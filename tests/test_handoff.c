/* SPDX-License-Identifier: MIT */
/*
 * test_handoff.c - 测试 lock handoff 行为
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

#define NUM_ITERATIONS 100000
#define NUM_PRODUCERS 2
#define NUM_CONSUMERS 2

/* 共享状态 */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint g_produced = 0;
static atomic_uint g_consumed = 0;
static volatile int g_running = 1;

/* 统计 */
struct thread_stats {
    uint64_t lock_count;
    uint64_t total_hold_ns;
    uint64_t max_hold_ns;
    uint64_t total_wait_ns;
    uint64_t max_wait_ns;
};

static struct thread_stats producer_stats[NUM_PRODUCERS];
static struct thread_stats consumer_stats[NUM_CONSUMERS];

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Producer: 模拟生产者，持锁时间较长 */
static void *producer_thread(void *arg)
{
    int id = *(int *)arg;
    struct thread_stats *stats = &producer_stats[id];
    
    /* 绑定 CPU */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(id * 2, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    
    memset(stats, 0, sizeof(*stats));
    
    while (g_running) {
        uint64_t wait_start = get_time_ns();
        
        pthread_mutex_lock(&g_mutex);
        
        uint64_t hold_start = get_time_ns();
        uint64_t wait_ns = hold_start - wait_start;
        
        /* 临界区：模拟生产工作 */
        unsigned int produced = atomic_load(&g_produced);
        if (produced < NUM_ITERATIONS) {
            /* 模拟一些工作 */
            for (volatile int i = 0; i < 100; i++);
            atomic_fetch_add(&g_produced, 1);
        }
        
        uint64_t hold_end = get_time_ns();
        pthread_mutex_unlock(&g_mutex);
        
        /* 更新统计 */
        uint64_t hold_ns = hold_end - hold_start;
        stats->lock_count++;
        stats->total_hold_ns += hold_ns;
        stats->total_wait_ns += wait_ns;
        if (hold_ns > stats->max_hold_ns)
            stats->max_hold_ns = hold_ns;
        if (wait_ns > stats->max_wait_ns)
            stats->max_wait_ns = wait_ns;
        
        if (produced >= NUM_ITERATIONS)
            break;
    }
    
    return NULL;
}

/* Consumer: 模拟消费者，持锁时间较短 */
static void *consumer_thread(void *arg)
{
    int id = *(int *)arg;
    struct thread_stats *stats = &consumer_stats[id];
    
    /* 绑定 CPU */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(id * 2 + 1, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    
    memset(stats, 0, sizeof(*stats));
    
    while (g_running) {
        uint64_t wait_start = get_time_ns();
        
        pthread_mutex_lock(&g_mutex);
        
        uint64_t hold_start = get_time_ns();
        uint64_t wait_ns = hold_start - wait_start;
        
        /* 临界区：模拟消费工作 */
        unsigned int consumed = atomic_load(&g_consumed);
        unsigned int produced = atomic_load(&g_produced);
        
        if (consumed < produced) {
            /* 模拟较短的工作 */
            for (volatile int i = 0; i < 20; i++);
            atomic_fetch_add(&g_consumed, 1);
        }
        
        uint64_t hold_end = get_time_ns();
        pthread_mutex_unlock(&g_mutex);
        
        /* 更新统计 */
        uint64_t hold_ns = hold_end - hold_start;
        stats->lock_count++;
        stats->total_hold_ns += hold_ns;
        stats->total_wait_ns += wait_ns;
        if (hold_ns > stats->max_hold_ns)
            stats->max_hold_ns = hold_ns;
        if (wait_ns > stats->max_wait_ns)
            stats->max_wait_ns = wait_ns;
        
        if (consumed >= NUM_ITERATIONS)
            break;
    }
    
    return NULL;
}

static void print_stats(const char *name, struct thread_stats *stats, int count)
{
    printf("\n%s Statistics:\n", name);
    for (int i = 0; i < count; i++) {
        struct thread_stats *s = &stats[i];
        double avg_hold = s->lock_count ? 
            (double)s->total_hold_ns / s->lock_count : 0;
        double avg_wait = s->lock_count ? 
            (double)s->total_wait_ns / s->lock_count : 0;
        
        printf("  [%d] locks=%lu, avg_hold=%.1fns, max_hold=%luns, "
               "avg_wait=%.1fns, max_wait=%luns\n",
               i, s->lock_count, avg_hold, s->max_hold_ns,
               avg_wait, s->max_wait_ns);
    }
}

int main(void)
{
    pthread_t producers[NUM_PRODUCERS];
    pthread_t consumers[NUM_CONSUMERS];
    int producer_ids[NUM_PRODUCERS];
    int consumer_ids[NUM_CONSUMERS];
    
    printf("=== Lock Handoff Test ===\n");
    printf("Iterations: %d\n", NUM_ITERATIONS);
    printf("Producers: %d, Consumers: %d\n", NUM_PRODUCERS, NUM_CONSUMERS);
    
    uint64_t start = get_time_ns();
    
    /* 启动线程 */
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        producer_ids[i] = i;
        pthread_create(&producers[i], NULL, producer_thread, &producer_ids[i]);
    }
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        consumer_ids[i] = i;
        pthread_create(&consumers[i], NULL, consumer_thread, &consumer_ids[i]);
    }
    
    /* 等待完成 */
    for (int i = 0; i < NUM_PRODUCERS; i++)
        pthread_join(producers[i], NULL);
    
    g_running = 0;
    
    for (int i = 0; i < NUM_CONSUMERS; i++)
        pthread_join(consumers[i], NULL);
    
    uint64_t end = get_time_ns();
    
    /* 打印结果 */
    printf("\nTotal time: %.2f ms\n", (end - start) / 1e6);
    printf("Produced: %u, Consumed: %u\n",
           atomic_load(&g_produced), atomic_load(&g_consumed));
    
    print_stats("Producer", producer_stats, NUM_PRODUCERS);
    print_stats("Consumer", consumer_stats, NUM_CONSUMERS);
    
    return 0;
}
