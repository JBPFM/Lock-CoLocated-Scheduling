// user_probe.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

/* include generated skeleton header */
#include "mutex_probe.skel.h"

/* BSD queue for simple list of perf entries */
#include <sys/queue.h>

static volatile sig_atomic_t exiting = 0;
static void sig_handler(int sig) { exiting = 1; }


/* event structure matching BPF-side lock_event */
struct lock_event {
    uint32_t pid;
    uint32_t tid;
    uint64_t mutex_addr;
    uint64_t ts_ns;
    uint64_t type; // 1=acquire,2=release
};

/* copy of BPF-side struct lock_state for userspace reads */
struct lock_state {
    uint64_t wait_start_ns;
    uint64_t acquire_ns;
    uint64_t hold_accum_ns;
    uint64_t wait_accum_ns;
    uint64_t preempt_count;
    uint64_t cs_exec_accum_ns;
    uint64_t acquires;
    uint64_t waits;
};

/* utility: dump tid_state_map periodically */
static void dump_tid_states(int map_fd)
{
    uint32_t key = 0, next_key;
    struct lock_state st;
    int res;

    /* iterate map using bpf_map_get_next_key */
    while (1) {
        res = bpf_map_get_next_key(map_fd, (const void *)&key, (void *)&next_key);
        if (res < 0) break;
        memset(&st, 0, sizeof(st));
        if (bpf_map_lookup_elem(map_fd, &next_key, &st) == 0) {
            printf("tid=%u acquires=%llu waits=%llu hold_accum_ns=%llu wait_accum_ns=%llu preempt=%llu\n",
                   next_key,
                   (unsigned long long)st.acquires,
                   (unsigned long long)st.waits,
                   (unsigned long long)st.hold_accum_ns,
                   (unsigned long long)st.wait_accum_ns,
                   (unsigned long long)st.preempt_count);
        }
        key = next_key;
    }
}

/* Helper to write filter maps (pid and comm) */
static void write_filter_maps(struct mutex_probe_bpf *skel, uint32_t filter_pid, const char *filter_comm)
{
    int map_fd;
    uint32_t map_key = 0;

    if (skel->maps.filter_pid_map) {
        map_fd = bpf_map__fd(skel->maps.filter_pid_map);
        if (map_fd >= 0) {
            if (bpf_map_update_elem(map_fd, &map_key, &filter_pid, BPF_ANY) != 0) {
                fprintf(stderr, "failed to write filter_pid_map: %s\n", strerror(errno));
            } else {
                printf("wrote filter_pid=%u\n", filter_pid);
            }
        }
    }

    if (skel->maps.filter_comm_map) {
        map_fd = bpf_map__fd(skel->maps.filter_comm_map);
        if (map_fd >= 0) {
            char buf[16] = {};
            if (filter_comm && filter_comm[0] != '\0') strncpy(buf, filter_comm, sizeof(buf)-1);
            if (bpf_map_update_elem(map_fd, &map_key, buf, BPF_ANY) != 0) {
                fprintf(stderr, "failed to write filter_comm_map: %s\n", strerror(errno));
            } else {
                printf("wrote filter_comm=%s\n", buf[0] ? buf : "<empty>");
            }
        }
    }
}

int main(int argc, char **argv)
{
    struct mutex_probe_bpf *skel = NULL;
    int err;
    uint32_t filter_pid = 0;
    char filter_comm[16] = {};
    int opt;

    while ((opt = getopt(argc, argv, "p:n:")) != -1) {
        switch (opt) {
            case 'p': filter_pid = (uint32_t)atoi(optarg); break;
            case 'n': strncpy(filter_comm, optarg, sizeof(filter_comm)-1); break;
            default:
                fprintf(stderr, "usage: %s [-p pid] [-n comm]\n", argv[0]);
                return 1;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* increase memlock for BPF */
    struct rlimit rl = {RLIM_INFINITY, RLIM_INFINITY};
    if (setrlimit(RLIMIT_MEMLOCK, &rl)) {
        fprintf(stderr, "setrlimit(RLIMIT_MEMLOCK) failed: %s\n", strerror(errno));
        /* continue, might still fail later */
    }

    /* open skeleton */
    skel = mutex_probe_bpf__open();
    if (!skel) {
        fprintf(stderr, "failed to open BPF skeleton\n");
        return 1;
    }

    /* load and verify BPF programs */
    err = mutex_probe_bpf__load(skel);
    if (err) {
        fprintf(stderr, "failed to load BPF skeleton: %d\n", err);
        goto cleanup;
    }

     const char *libc_path = "/lib/x86_64-linux-gnu/libc.so.6";

    //  /* ---------- pthread_mutex_lock (entry) ---------- */
    // struct bpf_uprobe_opts opts_lock = {};
    // opts_lock.sz = sizeof(opts_lock);
    // opts_lock.retprobe = false;                /* entry probe */
    // opts_lock.func_name = "pthread_mutex_lock@GLIBC_2.2.5";/* 指定符号名 */
    // opts_lock.attach_mode = 0;                 /* 默认挂载模式 */

    // skel->links.uprobe_pthread_mutex_lock =
    //     bpf_program__attach_uprobe_opts(
    //         skel->progs.uprobe_pthread_mutex_lock,
    //         -1,          
    //         libc_path,       /* binary/shared-library path */
    //         0,               /* func_offset: 0 => function entry (func_name used) */
    //         &opts_lock
    //     );

    // if (!skel->links.uprobe_pthread_mutex_lock) {
    //     err = -errno ?: -EINVAL;
    //     fprintf(stderr, "ERROR: attach uprobe pthread_mutex_lock failed: %d (%s)\n",
    //             err, strerror(-err));
    // }

    // /* ---------- pthread_mutex_lock (return) uretprobe ---------- */
    // struct bpf_uprobe_opts opts_lock_ret = {};
    // opts_lock_ret.sz = sizeof(opts_lock_ret);
    // opts_lock_ret.retprobe = true;             /* mark as return probe */
    // opts_lock_ret.func_name = "pthread_mutex_lock@GLIBC_2.2.5";
    // opts_lock_ret.attach_mode = 0;

    // skel->links.uretprobe_pthread_mutex_lock =
    //     bpf_program__attach_uprobe_opts(
    //         skel->progs.uretprobe_pthread_mutex_lock,
    //         -1,            
    //         libc_path,
    //         0,
    //         &opts_lock_ret
    //     );

    // if (!skel->links.uretprobe_pthread_mutex_lock) {
    //     err = -errno ?: -EINVAL;
    //     fprintf(stderr, "ERROR: attach uretprobe pthread_mutex_lock failed: %d (%s)\n",
    //             err, strerror(-err));
    // }

    /* ---------- pthread_mutex_unlock (entry) ---------- */
    struct bpf_uprobe_opts opts_unlock = {};
    opts_unlock.sz = sizeof(opts_unlock);
    opts_unlock.retprobe = false;
    opts_unlock.func_name = "pthread_mutex_unlock@GLIBC_2.2.5";
    opts_unlock.attach_mode = 0;

    skel->links.uprobe_pthread_mutex_unlock =
        bpf_program__attach_uprobe_opts(
            skel->progs.uprobe_pthread_mutex_unlock,
            -1,
            libc_path,
            0,
            &opts_unlock
        );

    if (!skel->links.uprobe_pthread_mutex_unlock) {
        err = -errno ?: -EINVAL;
        fprintf(stderr, "ERROR: attach uprobe pthread_mutex_unlock failed: %d (%s)\n",
                err, strerror(-err));
    }

    skel->links.kprobe__x64_sys_futex = bpf_program__attach_kprobe(skel->progs.kprobe__x64_sys_futex, false, "__x64_sys_futex");
    skel->links.kretprobe__x64_sys_futex = bpf_program__attach_kprobe(skel->progs.kretprobe__x64_sys_futex, true, "__x64_sys_futex");
    skel->links.trace_sched_switch = bpf_program__attach_tracepoint(skel->progs.trace_sched_switch, "sched", "sched_switch");

    // if (!skel->links.uprobe_pthread_mutex_lock || !skel->links.uretprobe_pthread_mutex_lock || !skel->links.uprobe_pthread_mutex_unlock ||
    //     !skel->links.kprobe__x64_sys_futex || !skel->links.kretprobe__x64_sys_futex || !skel->links.trace_sched_switch) {
    //     fprintf(stderr, "failed to attach one or more probes; check library path and permissions\n");
    //     goto cleanup;
    // }

    /* write filter maps */
    write_filter_maps(skel, filter_pid, filter_comm);

    printf("loaded, tracing... filter_pid=%u filter_comm=%s (Ctrl-C to stop)\n",
           filter_pid, filter_comm[0] ? filter_comm : "<none>");

    /* prepare tid_state_map fd for periodic dump */
    int tid_state_map_fd = -1;
    if (skel->maps.tid_state_map) tid_state_map_fd = bpf_map__fd(skel->maps.tid_state_map);

    /* main loop */
    const int dump_interval_ms = 2000;
    int poll_ms = 200;
    struct timespec req, rem;
    req.tv_sec = poll_ms / 1000;
    req.tv_nsec = (poll_ms % 1000) * 1000000;

    struct timespec last_ts, cur_ts;
    clock_gettime(CLOCK_MONOTONIC, &last_ts);

    while (!exiting) {
        /* sleep for poll_ms (avoids busy-loop) */
        if (nanosleep(&req, &rem) == -1 && errno != EINTR) {
            /* ignore other errors */
        }

        /* compute real elapsed ms to avoid drift */
        clock_gettime(CLOCK_MONOTONIC, &cur_ts);
        long delta_ms = (cur_ts.tv_sec - last_ts.tv_sec) * 1000 +
                        (cur_ts.tv_nsec - last_ts.tv_nsec) / 1000000;
        last_ts = cur_ts;

        static long accum_ms = 0;
        accum_ms += delta_ms;
        if (accum_ms >= dump_interval_ms) {
            accum_ms = 0;
            if (tid_state_map_fd >= 0) {
                printf("---- tid_state_map dump ----\n");
                dump_tid_states(tid_state_map_fd);
                printf("---- end dump ----\n");
            }
        }
    }

    printf("exiting...\n");

cleanup:
    if (skel) mutex_probe_bpf__destroy(skel);
    return 0;
}
