/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_lhandoff - sched_ext 调度器
 * 实现 waiter 定向 dispatch 和 IN_CS owner 偏置
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

char _license[] SEC("license") = "GPL";

/* ========== 配置常量 ========== */
#define CACHELINE_SIZE          64
#define LH_LOCK_TABLE_BUCKETS   1024
#define LH_WAITER_TABLE_SLOTS   4096
#define LH_CS_TABLE_SLOTS       4096
#define LH_MAX_ALLOWED_TGIDS    256

#define LH_SLICE_NORMAL_NS      (5 * 1000 * 1000)
#define LH_SLICE_IN_CS_MULT     4
#define LH_SLICE_WAITER_NS      (1 * 1000 * 1000)

#define LH_DSQ_NORMAL           0
#define LH_DSQ_LOCKWAIT_BASE    1000

#define LH_WAITER_INACTIVE      0
#define LH_WAITER_ACTIVE        1

#define SCX_ENQ_PREEMPT         0x1ULL

/* 内置 DSQ IDs (from vmlinux.h scx_dsq_id_flags) */
#define SCX_DSQ_FLAG_BUILTIN    0x8000000000000000ULL
#define SCX_DSQ_GLOBAL          0x8000000000000001ULL
#define SCX_DSQ_LOCAL           0x8000000000000002ULL
#define SCX_DSQ_LOCAL_ON        0xC000000000000000ULL
#define SCX_DSQ_LOCAL_CPU_MASK  0x00000000FFFFFFFFULL

#define LH_DSQ_LOCKWAIT(cpu)    (LH_DSQ_LOCKWAIT_BASE + (cpu))

/* ========== 数据结构 ========== */
struct lh_lock_entry {
    u32 tag;
    u32 owner_tid;
    s32 owner_cpu;
    u32 gen;
    u64 t_start_ns;
    u8  pad[CACHELINE_SIZE - (4 + 4 + 4 + 4 + 8)];
};

struct lh_lock_bucket {
    struct lh_lock_entry way[2];
};

struct lh_waiter_slot {
    u32 flags;
    u32 tid;
    u64 lock_addr;
    s32 target_cpu;
    u32 pad0;
    u8  pad[CACHELINE_SIZE - (4 + 4 + 8 + 4 + 4)];
};

struct lh_cs_slot {
    u32 in_cs;
    u32 pad;
    u8  pad2[CACHELINE_SIZE - 8];
};

/* ========== BPF Maps ========== */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, LH_MAX_ALLOWED_TGIDS);
    __type(key, u32);
    __type(value, u8);
} allowed_tgids SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, LH_LOCK_TABLE_BUCKETS);
    __type(key, u32);
    __type(value, struct lh_lock_bucket);
    __uint(map_flags, BPF_F_MMAPABLE);
} lock_table SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, LH_WAITER_TABLE_SLOTS);
    __type(key, u32);
    __type(value, struct lh_waiter_slot);
    __uint(map_flags, BPF_F_MMAPABLE);
} waiter_table SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, LH_CS_TABLE_SLOTS);
    __type(key, u32);
    __type(value, struct lh_cs_slot);
    __uint(map_flags, BPF_F_MMAPABLE);
} cs_table SEC(".maps");

/* task_storage: 缓存 controlled 状态 */
struct task_ctx {
    bool controlled;
    bool checked;
};

struct {
    __uint(type, BPF_MAP_TYPE_TASK_STORAGE);
    __uint(map_flags, BPF_F_NO_PREALLOC);
    __type(key, int);
    __type(value, struct task_ctx);
} task_ctx_map SEC(".maps");

/* ========== 全局变量 ========== */
const volatile u32 nr_cpus = 1;
const volatile u64 hash_salt = 0x12345678deadbeef;

/* ========== 辅助宏 ========== */
#define LH_BUCKET_IDX(lock_addr) \
    (((u32)((lock_addr) ^ hash_salt) * 2654435761u) % LH_LOCK_TABLE_BUCKETS)

#define LH_TAG_FROM_ADDR(lock_addr) \
    ((u32)(((lock_addr) ^ hash_salt) >> 32) | 1)

#define LH_WAITER_SLOT_IDX(tid) ((tid) % LH_WAITER_TABLE_SLOTS)
#define LH_CS_SLOT_IDX(tid)     ((tid) % LH_CS_TABLE_SLOTS)

/* ========== 辅助函数 ========== */
static __always_inline bool is_task_controlled(struct task_struct *p)
{
    struct task_ctx *ctx;
    u32 tgid;
    u8 *allowed;

    ctx = bpf_task_storage_get(&task_ctx_map, p, NULL, 0);
    if (ctx && ctx->checked)
        return ctx->controlled;

    tgid = BPF_CORE_READ(p, tgid);
    allowed = bpf_map_lookup_elem(&allowed_tgids, &tgid);

    ctx = bpf_task_storage_get(&task_ctx_map, p, NULL,
                               BPF_LOCAL_STORAGE_GET_F_CREATE);
    if (ctx) {
        ctx->controlled = (allowed != NULL);
        ctx->checked = true;
    }

    return allowed != NULL;
}

static __always_inline s32 get_waiter_target_cpu(struct task_struct *p)
{
    u32 tid = BPF_CORE_READ(p, pid);
    u32 slot_idx = LH_WAITER_SLOT_IDX(tid);
    struct lh_waiter_slot *slot;

    slot = bpf_map_lookup_elem(&waiter_table, &slot_idx);
    if (!slot)
        return -1;

    if (slot->flags != LH_WAITER_ACTIVE)
        return -1;

    if (slot->tid != tid)
        return -1;

    if (slot->target_cpu >= 0 && slot->target_cpu < (s32)nr_cpus)
        return slot->target_cpu;

    if (slot->lock_addr != 0) {
        u32 bucket_idx = LH_BUCKET_IDX(slot->lock_addr);
        u32 tag = LH_TAG_FROM_ADDR(slot->lock_addr);
        struct lh_lock_bucket *bucket;

        bucket = bpf_map_lookup_elem(&lock_table, &bucket_idx);
        if (bucket) {
            if (bucket->way[0].tag == tag) {
                s32 cpu = bucket->way[0].owner_cpu;
                if (cpu >= 0 && cpu < (s32)nr_cpus)
                    return cpu;
            }
            if (bucket->way[1].tag == tag) {
                s32 cpu = bucket->way[1].owner_cpu;
                if (cpu >= 0 && cpu < (s32)nr_cpus)
                    return cpu;
            }
        }
    }

    return -1;
}

static __always_inline bool is_task_in_cs(struct task_struct *p)
{
    u32 tid = BPF_CORE_READ(p, pid);
    u32 slot_idx = LH_CS_SLOT_IDX(tid);
    struct lh_cs_slot *slot;

    slot = bpf_map_lookup_elem(&cs_table, &slot_idx);
    if (!slot)
        return false;

    return slot->in_cs != 0;
}

/* ========== sched_ext ops ========== */
SEC("struct_ops/lhandoff_select_cpu")
s32 BPF_PROG(lhandoff_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
    /* 确保 prev_cpu 有效 */
    if (prev_cpu < 0 || prev_cpu >= (s32)nr_cpus)
        prev_cpu = 0;

    if (!is_task_controlled(p))
        return prev_cpu;

    /* IN_CS owner: 保持在当前 CPU */
    if (is_task_in_cs(p))
        return prev_cpu;

    /* waiter: 尝试定向到 owner CPU */
    s32 target = get_waiter_target_cpu(p);
    if (target >= 0 && target < (s32)nr_cpus)
        return target;

    return prev_cpu;
}

SEC("struct_ops/lhandoff_enqueue")
void BPF_PROG(lhandoff_enqueue, struct task_struct *p, u64 enq_flags)
{
    u64 slice = LH_SLICE_NORMAL_NS;

    if (!is_task_controlled(p)) {
        /* 非受控任务：使用 global DSQ */
        scx_bpf_dsq_insert(p, SCX_DSQ_GLOBAL, LH_SLICE_NORMAL_NS, 0);
        return;
    }

    /* 检查是否是 waiter */
    s32 target_cpu = get_waiter_target_cpu(p);
    if (target_cpu >= 0) {
        /* waiter: 短 slice，dispatch 到 global DSQ 但设置 PREEMPT */
        scx_bpf_dsq_insert(p, SCX_DSQ_GLOBAL, LH_SLICE_WAITER_NS, SCX_ENQ_PREEMPT);
        return;
    }

    /* IN_CS owner: 更长 slice */
    if (is_task_in_cs(p)) {
        slice = LH_SLICE_NORMAL_NS * LH_SLICE_IN_CS_MULT;
    }

    scx_bpf_dsq_insert(p, SCX_DSQ_GLOBAL, slice, 0);
}

/* 不实现 dispatch - 让内核使用默认行为 */

SEC("struct_ops.s/lhandoff_init")
s32 BPF_PROG(lhandoff_init)
{
    /* 使用内置的 SCX_DSQ_GLOBAL，不创建自定义 DSQ */
    /* 简化版本：所有任务都使用 global DSQ */
    return 0;
}

SEC("struct_ops/lhandoff_exit")
void BPF_PROG(lhandoff_exit, struct scx_exit_info *ei)
{
}

/* ========== fork tracepoint ========== */
SEC("tp_btf/sched_process_fork")
int BPF_PROG(handle_fork, struct task_struct *parent, struct task_struct *child)
{
    u32 parent_tgid = BPF_CORE_READ(parent, tgid);
    u32 child_tgid = BPF_CORE_READ(child, tgid);
    u8 *allowed;
    u8 val = 1;

    allowed = bpf_map_lookup_elem(&allowed_tgids, &parent_tgid);
    if (allowed) {
        bpf_map_update_elem(&allowed_tgids, &child_tgid, &val, BPF_ANY);
    }

    return 0;
}

/* ========== sched_ext ops 结构体 ========== */
SEC(".struct_ops.link")
struct sched_ext_ops lhandoff_ops = {
    .select_cpu     = (void *)lhandoff_select_cpu,
    .enqueue        = (void *)lhandoff_enqueue,
    .init           = (void *)lhandoff_init,
    .exit           = (void *)lhandoff_exit,
    .name           = "lhandoff",
};
