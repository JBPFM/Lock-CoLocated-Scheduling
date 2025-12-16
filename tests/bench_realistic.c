/* SPDX-License-Identifier: MIT */
/*
 * bench_realistic.c - 更真实的锁竞争场景测试
 * 
 * 测试场景：
 * 1. 较长临界区 - 体现 IN_CS 偏置的价值
 * 2. 跨 CPU 竞争 - 体现 waiter 定向的价值
 * 3. 高竞争度 - 体现 handoff 的价值
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

#define NUM_THREADS     8
#define ITERATIONS      100000
#define CS_WORK_US      5       /* 临界区工作时间 (微秒) */

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t g_shared_data[64];  /* 共享数据，模拟真实工作 */
static atomic_uint g_completed = 0;

/* 统计 */
struct thread_stats {
    uint64_t lock_acquires;
    uint64_t total_wait_ns;
    uint64_t max_wait_ns;
    uint64_t total_hold_ns;
    uint64_t preempt_count;     /* 临界区内被调度次数估计 */
};

static struct thread_stats g_stats[NUM_THREADS];

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* 模拟临界区内的真实工作 */
static void do_critical_section_work(int id)
{
    /* 读写共享数据，模拟真实的临界区操作 */
    for (int i = 0; i < 64; i++) {
        g_shared_data[i] += id + i;
    }
    
    /* 模拟一些计算工作 */
    volatile uint64_t sum = 0;
    for (int i = 0; i < CS_WORK_US * 100; i++) {
        sum += i * i;
    }
    (void)sum;
}

/* 场景1: 长临界区 + 高竞争 */
static void *long_cs_thread(void *arg)
{
    int id = *(int *)arg;
    struct thread_stats *stats = &g_stats[id];
    int iters = ITERATIONS / NUM_THREADS;
    
    /* 分散到不同 CPU */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    
    memset(stats, 0, sizeof(*stats));
    
    for (int i = 0; i < iters; i++) {
        uint64_t wait_start = get_time_ns();
        
        pthread_mutex_lock(&g_mutex);
        
        uint64_t hold_start = get_time_ns();
        uint64_t wait_ns = hold_start - wait_start;
        
        /* 长临界区工作 */
        do_critical_section_work(id);
        
        uint64_t hold_end = get_time_ns();
        uint64_t hold_ns = hold_end - hold_start;
        
        pthread_mutex_unlock(&g_mutex);
        
        /* 更新统计 */
        stats->lock_acquires++;
        stats->total_wait_ns += wait_ns;
        stats->total_hold_ns += hold_ns;
        if (wait_ns > stats->max_wait_ns)
            stats->max_wait_ns = wait_ns;
        
        /* 检测可能的临界区内调度（hold 时间异常长） */
        if (hold_ns > CS_WORK_US * 1000 * 10) {  /* 超过预期10倍 */
            stats->preempt_count++;
        }
    }
    
    atomic_fetch_add(&g_completed, 1);
    return NULL;
}

/* 场景2: 乒乓式 handoff（两线程交替持锁） */
static pthread_mutex_t pingpong_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int pingpong_turn = 0;
static atomic_int pingpong_done = 0;

static void *pingpong_thread(void *arg)
{
    int id = *(int *)arg;
    int count = 0;
    uint64_t total_switch_ns = 0;
    
    /* 绑定到不同 CPU */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(id * 2, &cpuset);  /* CPU 0 和 CPU 2 */
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    
    uint64_t last_release = get_time_ns();
    
    while (!atomic_load(&pingpong_done)) {
        pthread_mutex_lock(&pingpong_mutex);
        
        uint64_t acquire_time = get_time_ns();
        
        if (atomic_load(&pingpong_turn) == id) {
            /* 记录从对方 release 到我 acquire 的时间 */
            if (count > 0) {
                total_switch_ns += acquire_time - last_release;
            }
            
            /* 做一点工作 */
            for (volatile int i = 0; i < 100; i++);
            
            count++;
            atomic_store(&pingpong_turn, 1 - id);
            
            if (count >= ITERATIONS / 2) {
                atomic_store(&pingpong_done, 1);
            }
            
            last_release = get_time_ns();
        }
        
        pthread_mutex_unlock(&pingpong_mutex);
        sched_yield();
    }
    
    if (count > 1) {
        printf("  Thread %d: %d switches, avg switch time: %.1f ns\n",
               id, count, (double)total_switch_ns / (count - 1));
    }
    
    return NULL;
}

/* 场景3: 突发竞争（多线程同时抢锁） */
static pthread_barrier_t burst_barrier;
static pthread_mutex_t burst_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *burst_thread(void *arg)
{
    int id = *(int *)arg;
    uint64_t total_wait = 0;
    int count = 0;
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    
    for (int round = 0; round < 1000; round++) {
        /* 所有线程同步，然后同时抢锁 */
        pthread_barrier_wait(&burst_barrier);
        
        uint64_t start = get_time_ns();
        pthread_mutex_lock(&burst_mutex);
        uint64_t acquired = get_time_ns();
        
        /* 短临界区 */
        for (volatile int i = 0; i < 50; i++);
        
        pthread_mutex_unlock(&burst_mutex);
        
        total_wait += acquired - start;
        count++;
    }
    
    g_stats[id].total_wait_ns = total_wait;
    g_stats[id].lock_acquires = count;
    
    return NULL;
}

static void run_long_cs_test(void)
{
    pthread_t threads[NUM_THREADS];
    int ids[NUM_THREADS];
    
    printf("\n=== Test 1: Long Critical Section (%d threads, %dus CS) ===\n",
           NUM_THREADS, CS_WORK_US);
    
    atomic_store(&g_completed, 0);
    uint64_t start = get_time_ns();
    
    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, long_cs_thread, &ids[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    uint64_t elapsed = get_time_ns() - start;
    
    /* 汇总统计 */
    uint64_t total_wait = 0, max_wait = 0, total_preempt = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        total_wait += g_stats[i].total_wait_ns;
        if (g_stats[i].max_wait_ns > max_wait)
            max_wait = g_stats[i].max_wait_ns;
        total_preempt += g_stats[i].preempt_count;
    }
    
    printf("Total time: %.2f ms\n", elapsed / 1e6);
    printf("Throughput: %.0f ops/sec\n", ITERATIONS * 1e9 / elapsed);
    printf("Avg wait: %.1f ns, Max wait: %lu ns\n",
           (double)total_wait / ITERATIONS, max_wait);
    printf("Estimated CS preemptions: %lu\n", total_preempt);
}

static void run_pingpong_test(void)
{
    pthread_t t1, t2;
    int id1 = 0, id2 = 1;
    
    printf("\n=== Test 2: Ping-Pong Handoff (cross-CPU) ===\n");
    
    atomic_store(&pingpong_turn, 0);
    atomic_store(&pingpong_done, 0);
    
    uint64_t start = get_time_ns();
    
    pthread_create(&t1, NULL, pingpong_thread, &id1);
    pthread_create(&t2, NULL, pingpong_thread, &id2);
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    
    uint64_t elapsed = get_time_ns() - start;
    printf("Total time: %.2f ms\n", elapsed / 1e6);
}

static void run_burst_test(void)
{
    pthread_t threads[NUM_THREADS];
    int ids[NUM_THREADS];
    
    printf("\n=== Test 3: Burst Contention (%d threads) ===\n", NUM_THREADS);
    
    pthread_barrier_init(&burst_barrier, NULL, NUM_THREADS);
    memset(g_stats, 0, sizeof(g_stats));
    
    uint64_t start = get_time_ns();
    
    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, burst_thread, &ids[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    uint64_t elapsed = get_time_ns() - start;
    
    pthread_barrier_destroy(&burst_barrier);
    
    /* 汇总 */
    uint64_t total_wait = 0;
    int total_count = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        total_wait += g_stats[i].total_wait_ns;
        total_count += g_stats[i].lock_acquires;
    }
    
    printf("Total time: %.2f ms\n", elapsed / 1e6);
    printf("Avg wait per lock: %.1f ns\n", (double)total_wait / total_count);
}

int main(void)
{
    printf("========================================\n");
    printf("Realistic Lock Contention Benchmark\n");
    printf("CPUs: %ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    printf("========================================\n");
    
    run_long_cs_test();
    run_pingpong_test();
    run_burst_test();
    
    return 0;
}
