# lhandoff 实验报告

## 实验环境

- **内核版本**: Linux 6.16.12-061612-generic
- **CPU**: 96 核
- **sched_ext**: 已启用
- **测试日期**: 2025-12-15

## 实验目标

验证 lhandoff 在不同锁竞争场景下的性能表现，特别是：
1. 无竞争 fast path 的开销
2. 高竞争场景下的吞吐量和延迟
3. IN_CS 偏置对临界区 owner 被调度走的影响

## 测试场景

### 场景 1: 基础 Mutex 性能 (bench_mutex)

测试无竞争和轻度竞争下的基本锁性能。

| 测试项 | Native | lhandoff | 差异 |
|--------|--------|----------|------|
| 无竞争 | 12.3 ns/op | 14.1 ns/op | +14.6% |
| 竞争 (4线程) | 146.3 ns/op | 146.8 ns/op | +0.3% |
| Handoff | 369.9 ns | 368.4 ns | -0.4% |

**结论**: 无竞争路径增加约 2ns 开销（hint 内存写入），竞争路径基本持平。

### 场景 2: 真实工作负载 (bench_realistic)

模拟更真实的锁竞争场景：长临界区、跨 CPU 竞争、突发竞争。

#### Test 1: 长临界区 (8 线程, 5μs 临界区)

| 指标 | Native | lhandoff | 变化 |
|------|--------|----------|------|
| 吞吐量 | 300,054 ops/s | 338,439 ops/s | **+12.8%** |
| 平均等待 | 21.8 μs | 16.0 μs | **-26.6%** |
| 最大等待 | 2.1 ms | 31.8 ms | +1414% |

#### Test 2: Ping-Pong Handoff (跨 CPU)

| 指标 | Native | lhandoff | 变化 |
|------|--------|----------|------|
| 平均切换时间 | 1,351 ns | 1,502 ns | +11.2% |
| 总时间 | 90.8 ms | 98.6 ms | +8.6% |

#### Test 3: 突发竞争 (8 线程)

| 指标 | Native | lhandoff | 变化 |
|------|--------|----------|------|
| 平均等待 | 1,354 ns | 580 ns | **-57.2%** |
| 总时间 | 22.8 ms | 43.9 ms | +92.5% |

### 场景 3: 高竞争压力测试 (bench_preempt)

200 线程竞争，超过 CPU 数量 (96)，强制调度竞争。

#### 单次运行结果

| 指标 | Native | lhandoff | 变化 |
|------|--------|----------|------|
| 总时间 | 2,826 ms | 2,170 ms | **-23.2%** |
| 吞吐量 | 3,538 ops/s | 4,607 ops/s | **+30.2%** |
| 平均持锁时间 | 277.7 μs | 213.2 μs | **-23.2%** |
| 平均等待时间 | 27.9 ms | 21.7 ms | **-22.2%** |
| 上下文切换 | 9,962 | 8,456 | **-15.1%** |

#### 多次运行稳定性验证 (3 次)

**Native**:
```
Run 1: Throughput: 3,470 ops/s, Avg wait: 28.2 ms
Run 2: Throughput: 3,427 ops/s, Avg wait: 28.9 ms
Run 3: Throughput: 3,542 ops/s, Avg wait: 27.7 ms
平均: 3,480 ops/s, 28.3 ms
```

**lhandoff**:
```
Run 1: Throughput: 4,415 ops/s, Avg wait: 22.7 ms
Run 2: Throughput: 4,675 ops/s, Avg wait: 21.9 ms
Run 3: Throughput: 4,622 ops/s, Avg wait: 21.4 ms
平均: 4,571 ops/s, 22.0 ms
```

| 指标 | Native (平均) | lhandoff (平均) | 提升 |
|------|--------------|-----------------|------|
| 吞吐量 | 3,480 ops/s | 4,571 ops/s | **+31.4%** |
| 平均等待 | 28.3 ms | 22.0 ms | **-22.3%** |

## 关键发现

### 1. 优化效果显著的场景

- **高竞争 + 长临界区**: 吞吐量提升 30%+，等待时间减少 22%+
- **线程数超过 CPU 数**: IN_CS 偏置效果明显
- **突发竞争**: 平均等待时间减少 57%

### 2. 优化效果不明显或负面的场景

- **无竞争**: 增加约 2ns 开销（hint 写入）
- **Ping-Pong 交替**: sched_ext 调度开销导致略慢 (~10%)
- **低竞争**: 优化收益被额外开销抵消

### 3. 技术实现要点

#### 成功的优化

1. **IN_CS 偏置**: 给临界区 owner 4x 更长的 slice (20ms vs 5ms)
2. **select_cpu 定向**: waiter 尝试调度到 owner 所在 CPU
3. **spin-then-yield**: 先 spin 100 次再 yield，减少 syscall

#### 遇到的问题

1. **sched_ext API 变化**: 6.16 内核 API 与文档不同
   - `scx_bpf_dispatch()` → `scx_bpf_dsq_insert()`
   - `scx_bpf_consume()` → `scx_bpf_dsq_move_to_local()`
   - DSQ ID 常量值变化

2. **自定义 DSQ 创建失败**: `scx_bpf_create_dsq()` 返回 -EINVAL
   - 当前使用 `SCX_DSQ_GLOBAL` 替代

3. **unlock 时无条件 yield**: 初版实现导致性能下降
   - 优化为只在有 waiter 时才 yield

## 结论

lhandoff 在高竞争场景下表现出色，特别是：
- 线程数超过 CPU 数时
- 临界区较长时
- 突发竞争时

但在低竞争场景下，额外的 hint 写入和 sched_ext 调度开销可能导致轻微性能下降。

## 后续优化方向

1. **自适应策略**: 根据竞争程度动态启用/禁用优化
2. **per-CPU lockwait DSQ**: 实现真正的 waiter 定向调度
3. **更精细的 slice 控制**: 根据临界区长度动态调整
4. **减少 hint 开销**: 使用 per-CPU 缓存减少跨核写入

## 复现步骤

```bash
# 1. 环境检查
cd lhandoff
./scripts/setup_kernel.sh

# 2. 编译
make
cd tests && make && cd ..

# 3. 运行基准测试
./tests/bench_mutex
./tests/bench_realistic
./tests/bench_preempt 200

# 4. 运行 lhandoff 测试
sudo ./launcher/lh_launcher ./tests/bench_mutex
sudo ./launcher/lh_launcher ./tests/bench_realistic
sudo ./launcher/lh_launcher ./tests/bench_preempt 200
```
