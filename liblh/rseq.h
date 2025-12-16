/* SPDX-License-Identifier: MIT */
/*
 * rseq.h - Restartable Sequences 支持
 * 用于零 syscall 获取当前 CPU ID
 */
#ifndef __LH_RSEQ_H
#define __LH_RSEQ_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/rseq.h>

#ifdef __x86_64__
#define RSEQ_SIG 0x53053053
#elif defined(__aarch64__)
#define RSEQ_SIG 0xd428bc00
#else
#define RSEQ_SIG 0
#endif

static __thread volatile struct rseq __rseq_abi __attribute__((tls_model("initial-exec")));
static __thread volatile bool __rseq_registered = false;

static inline int rseq_register(void)
{
    if (__rseq_registered)
        return 0;
    int ret = syscall(__NR_rseq, &__rseq_abi, sizeof(__rseq_abi), 0, RSEQ_SIG);
    if (ret == 0)
        __rseq_registered = true;
    return ret;
}

/* 获取当前 CPU ID（零 syscall） */
static inline int32_t rseq_cpu_id(void)
{
    if (!__rseq_registered)
        rseq_register();
    return __rseq_abi.cpu_id;
}

#endif /* __LH_RSEQ_H */
