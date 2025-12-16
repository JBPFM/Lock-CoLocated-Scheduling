/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_common.bpf.h - sched_ext 公共定义
 */
#ifndef __SCX_COMMON_BPF_H
#define __SCX_COMMON_BPF_H

/* sched_ext enqueue flags */
#ifndef SCX_ENQ_PREEMPT
#define SCX_ENQ_PREEMPT     0x1ULL
#endif

#ifndef SCX_ENQ_WAKEUP
#define SCX_ENQ_WAKEUP      0x2ULL
#endif

/* DSQ 相关 */
#ifndef SCX_DSQ_LOCAL
#define SCX_DSQ_LOCAL       0xFFFFFFFFFFFFFFFFULL
#endif

#ifndef SCX_DSQ_GLOBAL
#define SCX_DSQ_GLOBAL      0xFFFFFFFFFFFFFFFEULL
#endif

/* slice 相关 */
#ifndef SCX_SLICE_DFL
#define SCX_SLICE_DFL       (20 * 1000 * 1000)  /* 20ms */
#endif

#ifndef SCX_SLICE_INF
#define SCX_SLICE_INF       (~0ULL)
#endif

#endif /* __SCX_COMMON_BPF_H */
