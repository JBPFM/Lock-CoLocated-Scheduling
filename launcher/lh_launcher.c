/* SPDX-License-Identifier: MIT */
/*
 * lh_launcher - 控制进程
 * fork+SIGSTOP → load scx → allowlist TGID → SIGCONT → wait
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include "../common/lh_shared.h"

/* 前向声明 - 避免 skeleton 兼容性问题 */
static struct bpf_object *g_obj = NULL;
static struct bpf_link *g_ops_link = NULL;
static struct bpf_link *g_fork_link = NULL;
static pid_t g_child_pid = -1;

/* Map fds */
static int g_allowed_tgids_fd = -1;
static int g_lock_table_fd = -1;
static int g_waiter_table_fd = -1;
static int g_cs_table_fd = -1;

static void cleanup(void)
{
    if (g_fork_link) {
        bpf_link__destroy(g_fork_link);
        g_fork_link = NULL;
    }
    if (g_ops_link) {
        bpf_link__destroy(g_ops_link);
        g_ops_link = NULL;
    }
    if (g_obj) {
        bpf_object__close(g_obj);
        g_obj = NULL;
    }
}

static void sig_handler(int sig)
{
    fprintf(stderr, "[launcher] Received signal %d, cleaning up...\n", sig);
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGKILL);
    }
    cleanup();
    exit(1);
}

static int get_nr_cpus(void)
{
    int nr = sysconf(_SC_NPROCESSORS_ONLN);
    return nr > 0 ? nr : 1;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
    if (level == LIBBPF_DEBUG)
        return 0;
    return vfprintf(stderr, format, args);
}

static int load_bpf(const char *bpf_path)
{
    struct bpf_program *prog;
    struct bpf_map *map;
    int err;

    libbpf_set_print(libbpf_print_fn);

    /* 打开 BPF object */
    g_obj = bpf_object__open(bpf_path);
    if (!g_obj) {
        fprintf(stderr, "[launcher] Failed to open BPF object: %s\n", bpf_path);
        return -1;
    }

    /* 设置全局变量 */
    map = bpf_object__find_map_by_name(g_obj, ".rodata");
    if (map) {
        /* 设置 nr_cpus 和 hash_salt */
        u32 nr_cpus = get_nr_cpus();
        u64 hash_salt = 0x12345678deadbeef;
        
        /* rodata 在 load 前设置 */
        struct {
            u32 nr_cpus;
            u32 pad;
            u64 hash_salt;
        } rodata = { nr_cpus, 0, hash_salt };
        
        err = bpf_map__set_initial_value(map, &rodata, sizeof(rodata));
        if (err) {
            fprintf(stderr, "[launcher] Warning: Failed to set rodata: %d\n", err);
        }
    }

    /* 加载 BPF 程序 */
    err = bpf_object__load(g_obj);
    if (err) {
        fprintf(stderr, "[launcher] Failed to load BPF object: %d\n", err);
        bpf_object__close(g_obj);
        g_obj = NULL;
        return err;
    }

    /* 获取 map fds */
    map = bpf_object__find_map_by_name(g_obj, "allowed_tgids");
    if (map) g_allowed_tgids_fd = bpf_map__fd(map);

    map = bpf_object__find_map_by_name(g_obj, "lock_table");
    if (map) g_lock_table_fd = bpf_map__fd(map);

    map = bpf_object__find_map_by_name(g_obj, "waiter_table");
    if (map) g_waiter_table_fd = bpf_map__fd(map);

    map = bpf_object__find_map_by_name(g_obj, "cs_table");
    if (map) g_cs_table_fd = bpf_map__fd(map);

    /* Attach struct_ops (sched_ext) */
    map = bpf_object__find_map_by_name(g_obj, "lhandoff_ops");
    if (map) {
        g_ops_link = bpf_map__attach_struct_ops(map);
        if (!g_ops_link) {
            err = -errno;
            fprintf(stderr, "[launcher] Failed to attach struct_ops: %d\n", err);
            cleanup();
            return err;
        }
        fprintf(stderr, "[launcher] sched_ext scheduler attached\n");
    } else {
        fprintf(stderr, "[launcher] Warning: lhandoff_ops map not found\n");
    }

    /* Attach fork tracepoint */
    prog = bpf_object__find_program_by_name(g_obj, "handle_fork");
    if (prog) {
        g_fork_link = bpf_program__attach(prog);
        if (!g_fork_link) {
            fprintf(stderr, "[launcher] Warning: Failed to attach fork tracepoint\n");
        }
    }

    return 0;
}

static int add_tgid_to_allowlist(pid_t tgid)
{
    u32 key = tgid;
    u8 val = 1;

    if (g_allowed_tgids_fd < 0) {
        fprintf(stderr, "[launcher] allowed_tgids map not available\n");
        return -1;
    }

    int err = bpf_map_update_elem(g_allowed_tgids_fd, &key, &val, BPF_ANY);
    if (err) {
        fprintf(stderr, "[launcher] Failed to add TGID %d: %d\n", tgid, err);
        return err;
    }

    fprintf(stderr, "[launcher] Added TGID %d to allowlist\n", tgid);
    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options] <program> [args...]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -b <path>   BPF object file (default: ./scx/scx_lhandoff.bpf.o)\n");
    fprintf(stderr, "  -l <path>   liblh.so path (default: ./liblh/liblh.so)\n");
    fprintf(stderr, "  -h          Show this help\n");
}

int main(int argc, char *argv[])
{
    const char *bpf_path = "./scx/scx_lhandoff.bpf.o";
    const char *liblh_path = "./liblh/liblh.so";
    int opt;

    /* 使用 '+' 前缀让 getopt 在遇到非选项参数时停止 */
    while ((opt = getopt(argc, argv, "+hb:l:")) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 'b':
            bpf_path = optarg;
            break;
        case 'l':
            liblh_path = optarg;
            break;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "[launcher] Error: No program specified\n");
        print_usage(argv[0]);
        return 1;
    }

    char *target_prog = argv[optind];
    char **target_argv = &argv[optind];

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Step 1: fork 子进程并暂停 */
    g_child_pid = fork();
    if (g_child_pid < 0) {
        perror("[launcher] fork");
        return 1;
    }

    if (g_child_pid == 0) {
        /* 子进程：SIGSTOP 自己 */
        raise(SIGSTOP);
        /* 被 SIGCONT 后执行目标程序 */
        execvp(target_prog, target_argv);
        perror("[launcher] execvp");
        _exit(127);
    }

    fprintf(stderr, "[launcher] Child PID: %d\n", g_child_pid);

    /* 等待子进程 SIGSTOP */
    int status;
    if (waitpid(g_child_pid, &status, WUNTRACED) < 0) {
        perror("[launcher] waitpid");
        return 1;
    }

    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "[launcher] Child did not stop\n");
        return 1;
    }

    fprintf(stderr, "[launcher] Child stopped, loading BPF...\n");

    /* Step 2: 加载 BPF */
    if (load_bpf(bpf_path) != 0) {
        kill(g_child_pid, SIGKILL);
        return 1;
    }

    /* Step 3: 添加 TGID 到 allowlist */
    if (add_tgid_to_allowlist(g_child_pid) != 0) {
        cleanup();
        kill(g_child_pid, SIGKILL);
        return 1;
    }

    /* 设置环境变量 */
    char env_buf[64];
    
    snprintf(env_buf, sizeof(env_buf), "%d", g_lock_table_fd);
    setenv("LH_LOCK_TABLE_FD", env_buf, 1);
    
    snprintf(env_buf, sizeof(env_buf), "%d", g_waiter_table_fd);
    setenv("LH_WAITER_TABLE_FD", env_buf, 1);
    
    snprintf(env_buf, sizeof(env_buf), "%d", g_cs_table_fd);
    setenv("LH_CS_TABLE_FD", env_buf, 1);
    
    setenv("LH_HASH_SALT", "12345678deadbeef", 1);
    setenv("LH_ENABLED", "1", 1);
    setenv("LD_PRELOAD", liblh_path, 1);

    fprintf(stderr, "[launcher] Resuming child...\n");

    /* Step 4: 恢复子进程 */
    if (kill(g_child_pid, SIGCONT) < 0) {
        perror("[launcher] SIGCONT");
        cleanup();
        return 1;
    }

    /* Step 5: 等待子进程完成 */
    while (1) {
        pid_t wpid = waitpid(-1, &status, 0);
        if (wpid < 0) {
            if (errno == ECHILD)
                break;
            perror("[launcher] waitpid");
            break;
        }

        if (wpid == g_child_pid) {
            if (WIFEXITED(status)) {
                int code = WEXITSTATUS(status);
                fprintf(stderr, "[launcher] Child exited: %d\n", code);
                cleanup();
                return code;
            } else if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                fprintf(stderr, "[launcher] Child killed by signal %d\n", sig);
                cleanup();
                return 128 + sig;
            }
        }
    }

    cleanup();
    return 0;
}
