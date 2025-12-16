/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * lhandoff - 共享数据结构定义
 * 用户态 (liblh.so) 和内核态 (scx_lhandoff) 共享
 */
#ifndef __LH_SHARED_H
#define __LH_SHARED_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdatomic.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;
#endif

#define CACHELINE_SIZE 64

/* ========== 配置参数 ========== */
#define LH_LOCK_TABLE_BUCKETS   1024    /* 2-way 组相联 bucket 数 */
#define LH_WAITER_TABLE_SLOTS   4096    /* waiter hint 表 slot 数 */
#define LH_CS_TABLE_SLOTS       4096    /* IN_CS 表 slot 数 */
#define LH_MAX_ALLOWED_TGIDS    256     /* 最大允许的 TGID 数 */

/* 降级策略参数 */
#define LH_YIELD_BUDGET         64      /* 最大 yield 次数 */
#define LH_FALLBACK_US          500     /* 超时回退阈值 (微秒) */

/* slice 配置 (纳秒) */
#define LH_SLICE_NORMAL_NS      (5 * 1000 * 1000)   /* 5ms */
#define LH_SLICE_IN_CS_MULT     4                    /* IN_CS 倍数 */
#define LH_SLICE_WAITER_NS      (1 * 1000 * 1000)   /* 1ms - waiter 短 slice */

/* DSQ IDs */
#define LH_DSQ_NORMAL           0
#define LH_DSQ_LOCKWAIT_BASE    1000    /* per-cpu: 1000 + cpu_id */

/* ========== waiter_slot flags ========== */
#define LH_WAITER_INACTIVE      0
#define LH_WAITER_ACTIVE        1

/* ========== lock_entry: 2-way 组相联 cacheline 对齐 ========== */
struct lh_lock_entry {
#ifdef __KERNEL__
    u32 tag;            /* 发布字段：最后 release store */
#else
    _Atomic u32 tag;
#endif
    u32 owner_tid;
    s32 owner_cpu;
    u32 gen;
    u64 t_start_ns;     /* 可选：用于降级决策 */
    u8  pad[CACHELINE_SIZE - (4 + 4 + 4 + 4 + 8)];
} __attribute__((aligned(CACHELINE_SIZE)));

struct lh_lock_bucket {
    struct lh_lock_entry way[2];
} __attribute__((aligned(CACHELINE_SIZE * 2)));

/* ========== waiter_slot: tid-index mmapable array ========== */
struct lh_waiter_slot {
#ifdef __KERNEL__
    u32 flags;          /* 0/ACTIVE，发布字段 */
#else
    _Atomic u32 flags;
#endif
    u32 tid;            /* 校验 */
    u64 lock_addr;      /* 或 (bucket, tag) */
    s32 target_cpu;     /* 可填 -1，由内核算 */
    u32 pad0;
    u8  pad[CACHELINE_SIZE - (4 + 4 + 8 + 4 + 4)];
} __attribute__((aligned(CACHELINE_SIZE)));

/* ========== cs_slot: IN_CS 表 ========== */
struct lh_cs_slot {
#ifdef __KERNEL__
    u32 in_cs;          /* 0/1 或 depth */
#else
    _Atomic u32 in_cs;
#endif
    u32 pad;
    u8  pad2[CACHELINE_SIZE - 8];
} __attribute__((aligned(CACHELINE_SIZE)));

/* ========== 辅助宏 ========== */
#define LH_BUCKET_IDX(lock_addr, salt) \
    (((u32)((lock_addr) ^ (salt)) * 2654435761u) % LH_LOCK_TABLE_BUCKETS)

#define LH_TAG_FROM_ADDR(lock_addr, salt) \
    ((u32)(((lock_addr) ^ (salt)) >> 32) | 1)  /* 确保非零 */

#define LH_WAITER_SLOT_IDX(tid) ((tid) % LH_WAITER_TABLE_SLOTS)
#define LH_CS_SLOT_IDX(tid)     ((tid) % LH_CS_TABLE_SLOTS)

#define LH_DSQ_LOCKWAIT(cpu)    (LH_DSQ_LOCKWAIT_BASE + (cpu))

#endif /* __LH_SHARED_H */
