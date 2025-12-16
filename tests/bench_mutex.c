/* SPDX-License-Identifier: MIT */
/*
 * bench_mutex.c - 简单的 mutex 性能测试
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

#define ITERATIONS 1000000
#define NUM_THREADS 4

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t g_counter = 0;

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* 无竞争测试 */
static void bench_uncontended(void)
{
    uint64_t start, end;
    pthread_mutex_t local_mutex = PTHREAD_MUTEX_INITIALIZER;

    start = get_time_ns();
    for (int i = 0; i < ITERATIONS; i++) {
        pthread_mutex_lock(&local_mutex);
        g_counter++;
        pthread_mutex_unlock(&local_mutex);
    }
    end = get_time_ns();

    double ns_per_op = (double)(end - start) / ITERATIONS;
    printf("Uncontended: %.2f ns/op\n", ns_per_op);
}

/* 竞争测试线程 */
static void *contended_thread(void *arg)
{
    int id = *(int *)arg;
    int iters = ITERATIONS / NUM_THREADS;

    /* 绑定到不同 CPU */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    for (int i = 0; i < iters; i++) {
        pthread_mutex_lock(&g_mutex);
        g_counter++;
        /* 模拟临界区工作 */
        for (volatile int j = 0; j < 10; j++);
        pthread_mutex_unlock(&g_mutex);
    }

    return NULL;
}

/* 竞争测试 */
static void bench_contended(void)
{
    pthread_t threads[NUM_THREADS];
    int ids[NUM_THREADS];
    uint64_t start, end;

    g_counter = 0;

    start = get_time_ns();
    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, contended_thread, &ids[i]);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    end = get_time_ns();

    double ns_per_op = (double)(end - start) / ITERATIONS;
    printf("Contended (%d threads): %.2f ns/op, counter=%lu\n",
           NUM_THREADS, ns_per_op, g_counter);
}

/* handoff 测试：模拟 owner-waiter 交替 */
static pthread_mutex_t handoff_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int handoff_turn = 0;
static volatile int handoff_done = 0;

static void *handoff_thread(void *arg)
{
    int id = *(int *)arg;
    int count = 0;

    /* 绑定到不同 CPU */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    while (!handoff_done) {
        pthread_mutex_lock(&handoff_mutex);
        if (handoff_turn == id) {
            count++;
            handoff_turn = 1 - id;  /* 交给另一个线程 */
            if (count >= ITERATIONS / 2) {
                handoff_done = 1;
            }
        }
        pthread_mutex_unlock(&handoff_mutex);
        sched_yield();
    }

    printf("Thread %d: %d handoffs\n", id, count);
    return NULL;
}

static void bench_handoff(void)
{
    pthread_t t1, t2;
    int id1 = 0, id2 = 1;
    uint64_t start, end;

    handoff_turn = 0;
    handoff_done = 0;

    start = get_time_ns();
    pthread_create(&t1, NULL, handoff_thread, &id1);
    pthread_create(&t2, NULL, handoff_thread, &id2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    end = get_time_ns();

    double ns_per_handoff = (double)(end - start) / ITERATIONS;
    printf("Handoff: %.2f ns/handoff\n", ns_per_handoff);
}

int main(int argc, char *argv[])
{
    printf("=== Mutex Benchmark ===\n");
    printf("Iterations: %d\n\n", ITERATIONS);

    printf("--- Uncontended ---\n");
    bench_uncontended();

    printf("\n--- Contended ---\n");
    bench_contended();

    printf("\n--- Handoff ---\n");
    bench_handoff();

    return 0;
}
