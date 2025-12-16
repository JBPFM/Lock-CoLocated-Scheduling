/* SPDX-License-Identifier: MIT */
/*
 * liblh.so - LD_PRELOAD 锁 shim
 * 拦截 pthread_mutex_lock/trylock/unlock，发布 hints 到共享内存
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sched.h>
#include <time.h>
#include <errno.h>

#include "../common/lh_shared.h"
#include "rseq.h"

/* ========== 配置常量 ========== */
#define SPIN_TRIES          100     /* trylock 前先 spin 的次数 */
#define SPIN_PAUSE_ITERS    10      /* 每次 spin pause 的迭代 */

/* ========== 真实函数指针 ========== */
static int (*real_pthread_mutex_lock)(pthread_mutex_t *) = NULL;
static int (*real_pthread_mutex_trylock)(pthread_mutex_t *) = NULL;
static int (*real_pthread_mutex_unlock)(pthread_mutex_t *) = NULL;

/* ========== 共享内存指针 ========== */
static struct lh_lock_bucket *g_lock_table = NULL;
static struct lh_waiter_slot *g_waiter_table = NULL;
static struct lh_cs_slot *g_cs_table = NULL;

/* ========== 配置 ========== */
static u64 g_hash_salt = 0x12345678deadbeef;
static int g_yield_budget = LH_YIELD_BUDGET;
static int g_fallback_us = LH_FALLBACK_US;
static bool g_initialized = false;
static bool g_enabled = true;

/* ========== TLS 缓存 ========== */
static __thread u32 tls_tid = 0;
static __thread bool tls_tid_cached = false;

/* ========== CPU pause 指令 ========== */
static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

/* ========== 辅助函数 ========== */

static inline u32 get_tid(void)
{
    if (!tls_tid_cached) {
        tls_tid = (u32)syscall(SYS_gettid);
        tls_tid_cached = true;
    }
    return tls_tid;
}

static inline s32 get_cpu(void)
{
    int32_t cpu = rseq_cpu_id();
    if (cpu >= 0)
        return cpu;
    return sched_getcpu();
}

static inline u64 get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ========== lock_table 操作 ========== */

static inline u32 bucket_idx(u64 lock_addr)
{
    return LH_BUCKET_IDX(lock_addr, g_hash_salt);
}

static inline u32 tag_from_addr(u64 lock_addr)
{
    return LH_TAG_FROM_ADDR(lock_addr, g_hash_salt);
}

static void lock_table_insert(u64 lock_addr, u32 tid, s32 cpu)
{
    if (!g_lock_table)
        return;

    u32 bidx = bucket_idx(lock_addr);
    u32 tag = tag_from_addr(lock_addr);
    struct lh_lock_bucket *bucket = &g_lock_table[bidx];

    for (int i = 0; i < 2; i++) {
        u32 old_tag = atomic_load_explicit(&bucket->way[i].tag,
                                           memory_order_acquire);
        if (old_tag == 0 || old_tag == tag) {
            bucket->way[i].owner_tid = tid;
            bucket->way[i].owner_cpu = cpu;
            bucket->way[i].gen++;
            bucket->way[i].t_start_ns = get_time_ns();
            atomic_store_explicit(&bucket->way[i].tag, tag,
                                  memory_order_release);
            return;
        }
    }

    bucket->way[0].owner_tid = tid;
    bucket->way[0].owner_cpu = cpu;
    bucket->way[0].gen++;
    bucket->way[0].t_start_ns = get_time_ns();
    atomic_store_explicit(&bucket->way[0].tag, tag, memory_order_release);
}

static void lock_table_remove(u64 lock_addr)
{
    if (!g_lock_table)
        return;

    u32 bidx = bucket_idx(lock_addr);
    u32 tag = tag_from_addr(lock_addr);
    struct lh_lock_bucket *bucket = &g_lock_table[bidx];

    for (int i = 0; i < 2; i++) {
        u32 old_tag = atomic_load_explicit(&bucket->way[i].tag,
                                           memory_order_acquire);
        if (old_tag == tag) {
            atomic_store_explicit(&bucket->way[i].tag, 0,
                                  memory_order_release);
            return;
        }
    }
}

static s32 lock_table_get_owner_cpu(u64 lock_addr)
{
    if (!g_lock_table)
        return -1;

    u32 bidx = bucket_idx(lock_addr);
    u32 tag = tag_from_addr(lock_addr);
    struct lh_lock_bucket *bucket = &g_lock_table[bidx];

    for (int i = 0; i < 2; i++) {
        u32 entry_tag = atomic_load_explicit(&bucket->way[i].tag,
                                             memory_order_acquire);
        if (entry_tag == tag) {
            return bucket->way[i].owner_cpu;
        }
    }
    return -1;
}

/* 检查是否有 waiter 在等待这个锁 */
static bool has_waiters_for_lock(u64 lock_addr)
{
    if (!g_waiter_table)
        return false;

    /* 简单检查：遍历部分 slot 看是否有人在等这个锁 */
    /* 这是 best-effort，不需要精确 */
    u32 start = (u32)(lock_addr >> 6) % LH_WAITER_TABLE_SLOTS;
    for (int i = 0; i < 16; i++) {
        u32 idx = (start + i) % LH_WAITER_TABLE_SLOTS;
        struct lh_waiter_slot *slot = &g_waiter_table[idx];
        if (atomic_load_explicit(&slot->flags, memory_order_acquire) == LH_WAITER_ACTIVE) {
            if (slot->lock_addr == lock_addr) {
                return true;
            }
        }
    }
    return false;
}

/* ========== waiter_table 操作 ========== */

static void waiter_slot_set(u32 tid, u64 lock_addr, s32 target_cpu)
{
    if (!g_waiter_table)
        return;

    u32 idx = LH_WAITER_SLOT_IDX(tid);
    struct lh_waiter_slot *slot = &g_waiter_table[idx];

    slot->tid = tid;
    slot->lock_addr = lock_addr;
    slot->target_cpu = target_cpu;
    atomic_store_explicit(&slot->flags, LH_WAITER_ACTIVE,
                          memory_order_release);
}

static void waiter_slot_clear(u32 tid)
{
    if (!g_waiter_table)
        return;

    u32 idx = LH_WAITER_SLOT_IDX(tid);
    struct lh_waiter_slot *slot = &g_waiter_table[idx];

    atomic_store_explicit(&slot->flags, LH_WAITER_INACTIVE,
                          memory_order_release);
}

/* ========== cs_table 操作 ========== */

static void cs_slot_enter(u32 tid)
{
    if (!g_cs_table)
        return;

    u32 idx = LH_CS_SLOT_IDX(tid);
    atomic_fetch_add_explicit(&g_cs_table[idx].in_cs, 1, memory_order_release);
}

static void cs_slot_leave(u32 tid)
{
    if (!g_cs_table)
        return;

    u32 idx = LH_CS_SLOT_IDX(tid);
    u32 old = atomic_fetch_sub_explicit(&g_cs_table[idx].in_cs, 1,
                                        memory_order_release);
    if (old == 0) {
        atomic_store_explicit(&g_cs_table[idx].in_cs, 0, memory_order_release);
    }
}

/* ========== hint 发布 ========== */

static void on_lock_acquired(pthread_mutex_t *mutex)
{
    u32 tid = get_tid();
    s32 cpu = get_cpu();
    u64 lock_addr = (u64)(uintptr_t)mutex;

    cs_slot_enter(tid);
    lock_table_insert(lock_addr, tid, cpu);
}

static void on_lock_release(pthread_mutex_t *mutex)
{
    u32 tid = get_tid();
    u64 lock_addr = (u64)(uintptr_t)mutex;

    cs_slot_leave(tid);
    lock_table_remove(lock_addr);
}

/* ========== 初始化 ========== */

static void init_real_funcs(void)
{
    real_pthread_mutex_lock = dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_pthread_mutex_trylock = dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_pthread_mutex_unlock = dlsym(RTLD_NEXT, "pthread_mutex_unlock");
}

static void init_shared_memory(void)
{
    const char *lock_fd_str = getenv("LH_LOCK_TABLE_FD");
    const char *waiter_fd_str = getenv("LH_WAITER_TABLE_FD");
    const char *cs_fd_str = getenv("LH_CS_TABLE_FD");
    const char *salt_str = getenv("LH_HASH_SALT");

    if (salt_str) {
        g_hash_salt = strtoull(salt_str, NULL, 16);
    }

    if (lock_fd_str) {
        int fd = atoi(lock_fd_str);
        size_t size = sizeof(struct lh_lock_bucket) * LH_LOCK_TABLE_BUCKETS;
        g_lock_table = mmap(NULL, size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
        if (g_lock_table == MAP_FAILED)
            g_lock_table = NULL;
    }

    if (waiter_fd_str) {
        int fd = atoi(waiter_fd_str);
        size_t size = sizeof(struct lh_waiter_slot) * LH_WAITER_TABLE_SLOTS;
        g_waiter_table = mmap(NULL, size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
        if (g_waiter_table == MAP_FAILED)
            g_waiter_table = NULL;
    }

    if (cs_fd_str) {
        int fd = atoi(cs_fd_str);
        size_t size = sizeof(struct lh_cs_slot) * LH_CS_TABLE_SLOTS;
        g_cs_table = mmap(NULL, size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
        if (g_cs_table == MAP_FAILED)
            g_cs_table = NULL;
    }

    const char *budget_str = getenv("LH_YIELD_BUDGET");
    if (budget_str)
        g_yield_budget = atoi(budget_str);

    const char *fallback_str = getenv("LH_FALLBACK_US");
    if (fallback_str)
        g_fallback_us = atoi(fallback_str);

    const char *enabled_str = getenv("LH_ENABLED");
    if (enabled_str && strcmp(enabled_str, "0") == 0)
        g_enabled = false;
}

__attribute__((constructor))
static void liblh_init(void)
{
    init_real_funcs();
    init_shared_memory();
    g_initialized = true;
}

/* ========== 拦截函数 ========== */

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (!g_initialized || !g_enabled || !real_pthread_mutex_lock) {
        int (*fn)(pthread_mutex_t *) = dlsym(RTLD_NEXT, "pthread_mutex_lock");
        return fn ? fn(mutex) : EINVAL;
    }

    /* Fast path: trylock */
    int ret = real_pthread_mutex_trylock(mutex);
    if (ret == 0) {
        on_lock_acquired(mutex);
        return 0;
    }

    /* 竞争路径 */
    u32 tid = get_tid();
    u64 lock_addr = (u64)(uintptr_t)mutex;
    u64 start_ns = get_time_ns();
    int yield_count = 0;
    int spin_count = 0;

    /* Phase 1: 先 spin 几次（不 yield） */
    while (spin_count < SPIN_TRIES) {
        for (int i = 0; i < SPIN_PAUSE_ITERS; i++) {
            cpu_relax();
        }
        spin_count++;

        ret = real_pthread_mutex_trylock(mutex);
        if (ret == 0) {
            on_lock_acquired(mutex);
            return 0;
        }
    }

    /* Phase 2: spin 失败，进入 yield 路径 */
    s32 target_cpu = lock_table_get_owner_cpu(lock_addr);
    waiter_slot_set(tid, lock_addr, target_cpu);

    while (1) {
        /* yield 让调度器把我们放到 owner CPU */
        sched_yield();
        yield_count++;

        /* 重试 trylock */
        ret = real_pthread_mutex_trylock(mutex);
        if (ret == 0) {
            waiter_slot_clear(tid);
            on_lock_acquired(mutex);
            return 0;
        }

        /* 更新 target_cpu（owner 可能迁移了） */
        target_cpu = lock_table_get_owner_cpu(lock_addr);
        if (g_waiter_table) {
            u32 idx = LH_WAITER_SLOT_IDX(tid);
            g_waiter_table[idx].target_cpu = target_cpu;
        }

        /* 降级检查 */
        u64 elapsed_us = (get_time_ns() - start_ns) / 1000;
        if (yield_count >= g_yield_budget || elapsed_us >= (u64)g_fallback_us) {
            waiter_slot_clear(tid);
            /* 回退到真实 pthread_mutex_lock */
            ret = real_pthread_mutex_lock(mutex);
            if (ret == 0) {
                on_lock_acquired(mutex);
            }
            return ret;
        }
    }
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    if (!g_initialized || !g_enabled || !real_pthread_mutex_trylock) {
        int (*fn)(pthread_mutex_t *) = dlsym(RTLD_NEXT, "pthread_mutex_trylock");
        return fn ? fn(mutex) : EINVAL;
    }

    int ret = real_pthread_mutex_trylock(mutex);
    if (ret == 0) {
        on_lock_acquired(mutex);
    }
    return ret;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    if (!g_initialized || !g_enabled || !real_pthread_mutex_unlock) {
        int (*fn)(pthread_mutex_t *) = dlsym(RTLD_NEXT, "pthread_mutex_unlock");
        return fn ? fn(mutex) : EINVAL;
    }

    u64 lock_addr = (u64)(uintptr_t)mutex;
    
    /* 检查是否有 waiter - 只有有 waiter 时才 yield */
    bool has_waiter = has_waiters_for_lock(lock_addr);

    /* 清理 hints */
    on_lock_release(mutex);

    /* 真实 unlock */
    int ret = real_pthread_mutex_unlock(mutex);

    /* 只有有 waiter 时才 yield 做 handoff */
    if (has_waiter) {
        sched_yield();
    }

    return ret;
}
