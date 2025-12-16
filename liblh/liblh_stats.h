/* SPDX-License-Identifier: MIT */
/*
 * liblh_stats.h - 运行时统计（可选，用于调试和性能分析）
 */
#ifndef __LIBLH_STATS_H
#define __LIBLH_STATS_H

#include <stdint.h>
#include <stdatomic.h>

struct lh_stats {
    _Atomic uint64_t lock_fast_path;      /* 无竞争 fast path 次数 */
    _Atomic uint64_t lock_yield_path;     /* yield 路径次数 */
    _Atomic uint64_t lock_fallback;       /* 降级到 futex 次数 */
    _Atomic uint64_t total_yields;        /* 总 yield 次数 */
    _Atomic uint64_t unlock_count;        /* unlock 次数 */
};

/* 全局统计（TLS 聚合） */
extern struct lh_stats g_lh_stats;

/* 统计宏（可通过编译选项禁用） */
#ifdef LH_ENABLE_STATS
#define LH_STAT_INC(field) \
    atomic_fetch_add_explicit(&g_lh_stats.field, 1, memory_order_relaxed)
#define LH_STAT_ADD(field, val) \
    atomic_fetch_add_explicit(&g_lh_stats.field, val, memory_order_relaxed)
#else
#define LH_STAT_INC(field) ((void)0)
#define LH_STAT_ADD(field, val) ((void)0)
#endif

/* 打印统计信息 */
void lh_print_stats(void);

#endif /* __LIBLH_STATS_H */
