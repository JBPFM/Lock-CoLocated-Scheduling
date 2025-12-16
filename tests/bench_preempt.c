/* SPDX-License-Identifier: MIT */
/*
 * bench_preempt.c - 测试临界区 owner 被调度走的场景
 * 
 * 这个测试专门设计来体现 IN_CS 偏置的价值：
 * - 长临界区（容易被调度走）
 * - 高线程数（超过 CPU 数，强制调度）
 * - 测量临界区内被调度的次数和影响
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <sys/resource.h>

/* 配置：线程数 > CPU 数，强制调度竞争 */
static int g_num_threads = 0;
static int g_num_cpus = 0;

#define ITERATIONS      10000
#define CS_WORK_LOOPS   50000   /* 较长的临界区工作 */

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint g_completed = 0;

struct thread_stats {
    uint64_t lock_acquires;
    uint64_t total_hold_ns;
    uint64_t max_hold_ns;
    uint64_t total_wait_ns;
    uint64_t max_wait_ns;
    uint64_t preempt_count;     /* 临界区内被调度次数 */
    uint64_t context_switches;  /* 总上下文切换次数 */
};

static struct thread_stats *g_stats = NULL;

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static uint64_t get_context_switches(void)
{
    struct rusage ru;
    getrusage(RUSAGE_THREAD, &ru);
    return ru.ru_nvcsw + ru.ru_nivcsw;
}

/* 长临界区工作 */
static volatile uint64_t g_dummy = 0;

static void do_long_critical_section(void)
{
    volatile uint64_t sum = 0;
    for (int i = 0; i < CS_WORK_LOOPS; i++) {
        sum += i;
    }
    g_dummy = sum;
}

static void *worker_thread(void *arg)
{
    int id = *(int *)arg;
    struct thread_stats *stats = &g_stats[id];
    int iters = ITERATIONS / g_num_threads;
    
    /* 不绑定 CPU，让调度器自由调度 */
    
    memset(stats, 0, sizeof(*stats));
    uint64_t cs_start = get_context_switches();
    
    /* 预期的临界区时间（用于检测 preemption） */
    uint64_t expected_hold_ns = 0;
    
    for (int i = 0; i < iters; i++) {
        uint64_t wait_start = get_time_ns();
        uint64_t cs_before = get_context_switches();
        
        pthread_mutex_lock(&g_mutex);
        
        uint64_t hold_start = get_time_ns();
        uint64_t wait_ns = hold_start - wait_start;
        
        /* 长临界区 */
        do_long_critical_section();
        
        uint64_t hold_end = get_time_ns();
        uint64_t hold_ns = hold_end - hold_start;
        uint64_t cs_after = get_context_switches();
        
        pthread_mutex_unlock(&g_mutex);
        
        /* 更新统计 */
        stats->lock_acquires++;
        stats->total_hold_ns += hold_ns;
        stats->total_wait_ns += wait_ns;
        
        if (hold_ns > stats->max_hold_ns)
            stats->max_hold_ns = hold_ns;
        if (wait_ns > stats->max_wait_ns)
            stats->max_wait_ns = wait_ns;
        
        /* 检测临界区内的上下文切换 */
        if (cs_after > cs_before) {
            stats->preempt_count++;
        }
        
        /* 更新预期时间（使用移动平均） */
        if (expected_hold_ns == 0) {
            expected_hold_ns = hold_ns;
        } else {
            expected_hold_ns = (expected_hold_ns * 7 + hold_ns) / 8;
        }
    }
    
    stats->context_switches = get_context_switches() - cs_start;
    atomic_fetch_add(&g_completed, 1);
    return NULL;
}

static void run_test(const char *name)
{
    pthread_t *threads = malloc(g_num_threads * sizeof(pthread_t));
    int *ids = malloc(g_num_threads * sizeof(int));
    
    printf("\n=== %s ===\n", name);
    printf("Threads: %d, CPUs: %d, Iterations: %d\n", 
           g_num_threads, g_num_cpus, ITERATIONS);
    
    atomic_store(&g_completed, 0);
    memset(g_stats, 0, g_num_threads * sizeof(struct thread_stats));
    
    uint64_t start = get_time_ns();
    
    for (int i = 0; i < g_num_threads; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, worker_thread, &ids[i]);
    }
    
    for (int i = 0; i < g_num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    uint64_t elapsed = get_time_ns() - start;
    
    /* 汇总统计 */
    uint64_t total_hold = 0, total_wait = 0;
    uint64_t max_hold = 0, max_wait = 0;
    uint64_t total_preempt = 0, total_cs = 0;
    int total_acquires = 0;
    
    for (int i = 0; i < g_num_threads; i++) {
        total_hold += g_stats[i].total_hold_ns;
        total_wait += g_stats[i].total_wait_ns;
        total_preempt += g_stats[i].preempt_count;
        total_cs += g_stats[i].context_switches;
        total_acquires += g_stats[i].lock_acquires;
        
        if (g_stats[i].max_hold_ns > max_hold)
            max_hold = g_stats[i].max_hold_ns;
        if (g_stats[i].max_wait_ns > max_wait)
            max_wait = g_stats[i].max_wait_ns;
    }
    
    printf("\nResults:\n");
    printf("  Total time: %.2f ms\n", elapsed / 1e6);
    printf("  Throughput: %.0f ops/sec\n", total_acquires * 1e9 / elapsed);
    printf("  Avg hold time: %.1f us\n", (double)total_hold / total_acquires / 1000);
    printf("  Max hold time: %.1f us\n", max_hold / 1000.0);
    printf("  Avg wait time: %.1f us\n", (double)total_wait / total_acquires / 1000);
    printf("  Max wait time: %.1f ms\n", max_wait / 1e6);
    printf("  CS preemptions: %lu (%.2f%%)\n", 
           total_preempt, 100.0 * total_preempt / total_acquires);
    printf("  Total context switches: %lu\n", total_cs);
    
    free(threads);
    free(ids);
}

int main(int argc, char *argv[])
{
    g_num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    
    /* 默认：线程数 = CPU 数 * 2，强制调度竞争 */
    g_num_threads = g_num_cpus * 2;
    if (g_num_threads > 256) g_num_threads = 256;
    
    if (argc > 1) {
        g_num_threads = atoi(argv[1]);
    }
    
    printf("========================================\n");
    printf("Preemption Test\n");
    printf("CPUs: %d, Threads: %d\n", g_num_cpus, g_num_threads);
    printf("========================================\n");
    
    g_stats = calloc(g_num_threads, sizeof(struct thread_stats));
    
    run_test("Long Critical Section with Oversubscription");
    
    free(g_stats);
    return 0;
}
