# lhandoff 设计文档

## 1. 核心目标

### 1.1 目标
- **无竞争 fast path 极低开销**: pthread_mutex_lock 无竞争仍接近原生（原子 CAS + 少量用户态 store），绝不引入 bpf() syscall
- **竞争路径 handoff 行为**: 抢锁失败线程不进入 futex wait 队列，而是写 waiter hint 后 sched_yield()
- **临界区 owner 抗调度**: 调度器对 IN_CS 线程强偏置（更长 slice、尽量不迁移）

### 1.2 非目标
- 不保证临界区绝对不被调度走
- 不改变 pthread mutex correctness
- 暂不做多租户安全隔离

## 2. 架构

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
└─────────────────────────┘     └─────────────────────────────────┘
              │                               │
              └───────────┬───────────────────┘
                          ▼
              ┌─────────────────────────┐
              │   Shared Memory (mmap)  │
              │  BPF_F_MMAPABLE arrays  │
              └─────────────────────────┘
```

## 3. 数据结构

### 3.1 lock_table (2-way 组相联)
```c
struct lh_lock_entry {
    _Atomic u32 tag;      // 发布字段
    u32 owner_tid;
    s32 owner_cpu;
    u32 gen;
    u64 t_start_ns;
} __attribute__((aligned(64)));

struct lh_lock_bucket {
    struct lh_lock_entry way[2];
};
```

### 3.2 waiter_table (tid-index)
```c
struct lh_waiter_slot {
    _Atomic u32 flags;    // INACTIVE/ACTIVE
    u32 tid;
    u64 lock_addr;
    s32 target_cpu;
} __attribute__((aligned(64)));
```

### 3.3 cs_table (tid-index)
```c
struct lh_cs_slot {
    _Atomic u32 in_cs;    // 0/1
} __attribute__((aligned(64)));
```

## 4. 关键路径

### 4.1 无竞争 fast path
```
pthread_mutex_lock(mutex):
  1. trylock() → 成功
  2. cs_slot[tid].in_cs = 1  (release store)
  3. lock_table 插入 owner 信息 (release store tag)
  4. 返回
```
**开销**: 原子 CAS + 2-3 次内存写，零 syscall

### 4.2 竞争路径
```
pthread_mutex_lock(mutex):
  1. trylock() → 失败
  2. 读 lock_table 获取 owner_cpu
  3. waiter_slot 写入 (release store flags)
  4. sched_yield()  ← 唯一 syscall
  5. waiter_slot.flags = 0
  6. 重试 trylock()
  7. 超过 budget/timeout → fallback 到真实 pthread_mutex_lock
```

### 4.3 unlock + handoff
```
pthread_mutex_unlock(mutex):
  1. cs_slot[tid].in_cs = 0
  2. lock_table 清除 entry
  3. 真实 pthread_mutex_unlock()
  4. sched_yield()  ← handoff 给 waiter
```

## 5. sched_ext 调度策略

### 5.1 enqueue
- 检查 waiter_slot → 定向 dispatch 到 owner_cpu 的 LOCKWAIT_DSQ
- 检查 cs_slot → IN_CS owner 使用更长 slice

### 5.2 dispatch
- 优先消费 LOCKWAIT_DSQ(cpu)
- 再消费 NORMAL_DSQ

### 5.3 select_cpu
- IN_CS owner: 返回 prev_cpu（减少迁移）
- waiter: 返回 target_cpu（定向）

## 6. 降级策略

为避免 yield 风暴，设置两个阈值：
- `LH_YIELD_BUDGET`: 最大 yield 次数（默认 32）
- `LH_FALLBACK_US`: 超时阈值（默认 100us）

超过任一阈值后，回退到真实 pthread_mutex_lock（进入 futex sleep）。
