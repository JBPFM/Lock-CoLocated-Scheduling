// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

struct lock_state {
    u64 wait_start_ns;
    u64 acquire_ns;
    u64 hold_accum_ns;
    u64 wait_accum_ns;
    u64 preempt_count;
    u64 cs_exec_accum_ns;
    u64 acquires;
    u64 waits;
};

/* maps */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16384);
    __type(key, u32);
    __type(value, struct lock_state);
} tid_state_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16384);
    __type(key, u32);
    __type(value, u64);
} tid_mutex_map SEC(".maps");

/* Filtering maps */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u32);
} filter_pid_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, char[16]);
} filter_comm_map SEC(".maps");

/* Helper: filter by pid/comm */
static __always_inline int passes_filter(void)
{
    u32 key = 0;
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 tid = pid_tgid & 0xFFFFFFFF;
    u32 tgid = pid_tgid >> 32;

    u32 *filter_pid = bpf_map_lookup_elem(&filter_pid_map, &key);
    if (filter_pid && *filter_pid != 0) {
        if (tgid != *filter_pid)
            return 0;
    }

    char *fcomm = bpf_map_lookup_elem(&filter_comm_map, &key);
    if (fcomm && fcomm[0] != 0) {
        char comm_buf[16];
        bpf_get_current_comm(&comm_buf, sizeof(comm_buf));
#pragma unroll
        for (int i = 0; i < 16; i++) {
            if (fcomm[i] != comm_buf[i])
                return 0;
            if (fcomm[i] == 0)
                break;
        }
    }

    return 1;
}

/* Uprobe for pthread_mutex_lock */
SEC("uprobe/pthread_mutex_lock")
int uprobe_pthread_mutex_lock(struct pt_regs *ctx)
{
    if (!passes_filter()) return 0;

    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 tid = pid_tgid & 0xFFFFFFFF;
    u64 ts = bpf_ktime_get_ns();
    struct lock_state zero = {};
    struct lock_state *st = bpf_map_lookup_elem(&tid_state_map, &tid);
    if (!st) {
        bpf_map_update_elem(&tid_state_map, &tid, &zero, BPF_ANY);
        st = bpf_map_lookup_elem(&tid_state_map, &tid);
        if (!st) return 0;
    }
    st->wait_start_ns = ts;
    st->waits++;
    return 0;
}

/* Uretprobe for pthread_mutex_lock */
SEC("uretprobe/pthread_mutex_lock")
int uretprobe_pthread_mutex_lock(struct pt_regs *ctx)
{
    if (!passes_filter()) return 0;

    int ret = PT_REGS_RC(ctx);
    if (ret != 0) return 0;

    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 tid = pid_tgid & 0xFFFFFFFF;
    u32 pid = pid_tgid >> 32;
    u64 ts = bpf_ktime_get_ns();
    struct lock_state *st = bpf_map_lookup_elem(&tid_state_map, &tid);
    if (!st) return 0;

    if (st->wait_start_ns != 0) {
        st->wait_accum_ns += ts - st->wait_start_ns;
        st->wait_start_ns = 0;
    }
    st->acquire_ns = ts;
    st->acquires++;

    u64 mutex_addr = 0;
#ifdef __TARGET_ARCH_x86
    mutex_addr = (u64)PT_REGS_PARM1(ctx);
#endif
    bpf_map_update_elem(&tid_mutex_map, &tid, &mutex_addr, BPF_ANY);
    return 0;
}

/* Uprobe for pthread_mutex_unlock */
SEC("uprobe/pthread_mutex_unlock")
int uprobe_pthread_mutex_unlock(struct pt_regs *ctx)
{
    if (!passes_filter()) return 0;

    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 tid = pid_tgid & 0xFFFFFFFF;
    u64 ts = bpf_ktime_get_ns();
    struct lock_state *st = bpf_map_lookup_elem(&tid_state_map, &tid);
    if (!st) return 0;

    if (st->acquire_ns != 0) {
        u64 hold = ts - st->acquire_ns;
        st->hold_accum_ns += hold;
        st->cs_exec_accum_ns += hold;
        st->acquire_ns = 0;
    }

    bpf_map_delete_elem(&tid_mutex_map, &tid);

    return 0;
}

/* kprobe/futex */
SEC("kprobe/__x64_sys_futex")
int kprobe__x64_sys_futex(struct pt_regs *ctx)
{
    if (!passes_filter()) return 0;
    u32 tid = bpf_get_current_pid_tgid() & 0xFFFFFFFF;
    u64 ts = bpf_ktime_get_ns();
    struct lock_state *st = bpf_map_lookup_elem(&tid_state_map, &tid);
    if (!st) return 0;
    if (st->wait_start_ns == 0 && st->acquire_ns == 0)
        st->wait_start_ns = ts;
    return 0;
}

SEC("kretprobe/__x64_sys_futex")
int kretprobe__x64_sys_futex(struct pt_regs *ctx)
{
    if (!passes_filter()) return 0;
    u32 tid = bpf_get_current_pid_tgid() & 0xFFFFFFFF;
    u64 ts = bpf_ktime_get_ns();
    struct lock_state *st = bpf_map_lookup_elem(&tid_state_map, &tid);
    if (!st) return 0;
    if (st->wait_start_ns != 0) {
        st->wait_accum_ns += ts - st->wait_start_ns;
        st->wait_start_ns = 0;
    }
    return 0;
}

/* sched_switch tracepoint */
SEC("tracepoint/sched/sched_switch")
int trace_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
    u32 prev_tid = ctx->prev_pid;
    u64 *mutex_addr = bpf_map_lookup_elem(&tid_mutex_map, &prev_tid);
    if (mutex_addr && *mutex_addr != 0) {
        struct lock_state *st = bpf_map_lookup_elem(&tid_state_map, &prev_tid);
        if (st) st->preempt_count++;
    }
    return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
