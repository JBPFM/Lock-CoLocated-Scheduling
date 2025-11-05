#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#define NUM_THREADS 4
#define INCREMENTS_PER_THREAD 1000000

// 共享资源：一个全局计数器
long long shared_counter = 0;

// 互斥锁
pthread_mutex_t counter_mutex;

/**
 * @brief 线程执行函数：重复锁定、增加计数器、解锁
 */
void *increment_routine(void *arg) {
    int i;
    long thread_id = (long)arg;

    printf("Thread %ld starting increments.\n", thread_id);

    for (i = 0; i < INCREMENTS_PER_THREAD; i++) {
        // ----------------------------------------------------
        // 关键点 1: 调用 pthread_mutex_lock
        if (pthread_mutex_lock(&counter_mutex) != 0) {
            perror("pthread_mutex_lock failed");
            exit(EXIT_FAILURE);
        }

        shared_counter++;

        // ----------------------------------------------------
        // 关键点 2: 调用 pthread_mutex_unlock
        if (pthread_mutex_unlock(&counter_mutex) != 0) {
            perror("pthread_mutex_unlock failed");
            exit(EXIT_FAILURE);
        }
    }

    printf("Thread %ld finished. Final value: %lld\n", thread_id, shared_counter);
    return NULL;
}

int main(void) {
    pthread_t threads[NUM_THREADS];
    int i;
    int expected_value = NUM_THREADS * INCREMENTS_PER_THREAD;

    printf("Starting mutex test with %d threads.\n", NUM_THREADS);
    printf("Expected final counter value: %d\n", expected_value);
    
    // ----------------------------------------------------
    // 初始化互斥锁
    if (pthread_mutex_init(&counter_mutex, NULL) != 0) {
        fprintf(stderr, "Mutex initialization failed: %s\n", strerror(errno));
        return 1;
    }

    // 创建线程
    for (i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, increment_routine, (void *)(long)(i + 1)) != 0) {
            fprintf(stderr, "Failed to create thread %d: %s\n", i, strerror(errno));
            return 1;
        }
    }
    
    printf("\n--- Resuming ---\n");

    // 等待所有线程结束
    for (i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // 销毁互斥锁
    if (pthread_mutex_destroy(&counter_mutex) != 0) {
        perror("pthread_mutex_destroy failed");
    }

    // 检查最终结果
    printf("\n--- Test Complete ---\n");
    printf("Final shared counter value: %lld\n", shared_counter);
    if (shared_counter == expected_value) {
        printf("Result: SUCCESS (No data race detected, Mutex worked)\n");
        return 0;
    } else {
        printf("Result: FAILURE (Should not happen with proper locking)\n");
        return 1;
    }
}