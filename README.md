# lhandoff - Lock Handoff with sched_ext

基于 sched_ext 的用户态锁优化方案，实现 CPU-local handoff 和临界区 owner 调度偏置。

## 架构

```
┌─────────────────────────────────────────────────────────────────┐
│                         launcher                                 │
│  fork+SIGSTOP → load scx → allowlist TGID → SIGCONT → wait      │
└─────────────────────────────────────────────────────────────────┘
                              │
              ┌───────────────┴───────────────┐
              ▼                               ▼
┌─────────────────────────┐     ┌─────────────────────────────────┐
│      liblh.so           │     │      scx_lhandoff (BPF)         │
│  LD_PRELOAD 锁 shim     │     │  sched_ext 调度器               │
│  - 拦截 pthread_mutex   │     │  - LOCKWAIT_DSQ per-cpu         │
│  - 发布 hints (mmap)    │     │  - waiter 定向 dispatch         │
│  - yield + 降级策略     │     │  - IN_CS owner 偏置             │
└─────────────────────────┘     └─────────────────────────────────┘
              │                               │
              └───────────┬───────────────────┘
                          ▼
              ┌─────────────────────────┐
              │   Shared Memory (mmap)  │
              │  - lock_table           │
              │  - waiter_table         │
              │  - cs_table             │
              └─────────────────────────┘
```

## 组件

- `launcher/` - 控制进程，负责 fork、加载 scx、管理 allowlist
- `liblh/` - LD_PRELOAD 库，拦截 pthread_mutex 并发布 hints
- `scx/` - sched_ext BPF 调度器
- `common/` - 共享数据结构定义

## 构建

```bash
make
```

## 使用

```bash
./launcher/lh_launcher ./your_program [args...]
```

## 设计原则

1. **Correctness 由原始 pthread mutex 保证** - 本项目只提供 best-effort 调度 hints
2. **Fast path 零 syscall** - hints 通过 mmap 共享内存发布，用户态只做内存写
3. **竞争路径 sched_yield() 是唯一必要 syscall**
4. **降级策略** - 超时/超次数回退真实 pthread_mutex_lock，避免 yield 风暴
