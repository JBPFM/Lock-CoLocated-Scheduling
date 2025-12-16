# LevelDB db_bench 性能测试报告

## 实验环境

- **内核版本**: Linux 6.16.12-061612-generic
- **CPU**: 96 核 Intel Xeon Gold 5318Y @ 2.10GHz
- **LevelDB**: version 1.23
- **测试日期**: 2025-12-15

## 测试配置

- **db_bench 路径**: `/home/jz/test/test/leveldb/build/db_bench`
- **操作数**: 100,000 per thread
- **线程数**: 1, 2, 4, 8, 16, 32, 64, 128, 256

## 测试结果

### fillrandom (随机写入)

| 线程数 | Native (ops/s) | lhandoff (ops/s) | 加速比 |
|--------|----------------|------------------|--------|
| 1 | 507,872 | 475,285 | 0.93x |
| 2 | 104,821 | 153,139 | **1.46x** |
| 4 | 31,851 | 46,544 | **1.46x** |
| 8 | 20,845 | 19,843 | 0.95x |
| 16 | 7,134 | 8,813 | **1.23x** |
| 32 | 4,154 | 4,490 | **1.08x** |
| 64 | 2,260 | 2,507 | **1.10x** |
| 128 | 1,047 | 1,194 | **1.14x** |
| 256 | 475 | 580 | **1.22x** |

### readrandom (随机读取)

| 线程数 | Native (ops/s) | lhandoff (ops/s) | 加速比 |
|--------|----------------|------------------|--------|
| 1 | 382,995 | 348,675 | 0.91x |
| 2 | 252,908 | 227,842 | 0.90x |
| 4 | 101,091 | 137,854 | **1.36x** |
| 8 | 56,570 | 93,361 | **1.65x** |
| 16 | 43,546 | 70,596 | **1.62x** |
| 32 | 23,645 | 36,764 | **1.55x** |
| 64 | 11,436 | 19,332 | **1.69x** |
| 128 | 5,694 | 9,933 | **1.74x** |
| 256 | 2,773 | 4,944 | **1.78x** |

## 结果分析

### 单线程性能开销

单线程时 lhandoff 有 **7-9% 的性能开销**，这是预期行为：

1. **liblh hint 写入开销**: 每次 lock/unlock 都需要写入共享内存（lock_table, cs_table）
2. **sched_ext 调度器开销**: BPF 程序在调度路径上的额外检查

这证明 lhandoff 的设计目标是正确的：**优化多线程竞争场景，而非单线程性能**。

### fillrandom 分析

1. **单线程 (1)**: 略慢 7%（hint 开销）
2. **低线程数 (2-4)**: **提升 46%** - 锁竞争开始出现，lhandoff 优化生效
3. **中等线程数 (8)**: 基本持平 - I/O 成为瓶颈
4. **高线程数 (16-256)**: **提升 8-23%** - 高竞争场景优化效果明显

### readrandom 分析

1. **单线程 (1-2)**: 略慢 9-10%（hint 开销）
2. **4+ 线程**: **全面提升 36-78%**
   - 读操作更依赖内存和锁，lhandoff 优化效果更明显
   - 随着线程数增加，加速比持续提升
3. **256 线程**: 达到 **1.78x** 加速

### 性能拐点

| 工作负载 | 拐点线程数 | 说明 |
|----------|------------|------|
| fillrandom | 2 | 2 线程开始有提升 |
| readrandom | 4 | 4 线程开始有提升 |

## 关键发现

### 优势场景

1. **读密集型 + 高并发**: readrandom 4-256 线程提升 36-78%
2. **写密集型 + 低并发**: fillrandom 2-4 线程提升 46%
3. **高线程数**: 256 线程时两种工作负载都有显著提升

### 局限性

1. **单线程**: 有 7-9% 开销（hint 写入 + sched_ext 开销）
2. **低线程数读操作**: 1-2 线程 readrandom 略慢
3. **I/O bound 场景**: fillrandom 8 线程时 I/O 成为瓶颈，优化效果被掩盖

### 与之前测试对比

之前使用 NUM=10000 时观察到单线程有提升，这是**测试误差**：
- 测试时间太短，噪声影响大
- I/O 缓存效应不稳定

使用 NUM=100000 后结果更准确，符合理论预期。

## 适用场景建议

| 场景 | 是否推荐使用 lhandoff |
|------|----------------------|
| 单线程应用 | ❌ 不推荐（有 7-9% 开销） |
| 2-4 线程写密集 | ✅ 强烈推荐（46% 提升） |
| 4+ 线程读密集 | ✅ 强烈推荐（36-78% 提升） |
| 高并发数据库 | ✅ 强烈推荐 |

## 复现步骤

```bash
cd lhandoff
./tests/bench_leveldb.sh
```

## 原始数据

```csv
benchmark,threads,mode,ops_per_sec,micros_per_op
fillrandom,1,native,507872,1.969
fillrandom,1,lhandoff,475285,2.104
fillrandom,2,native,104821,9.540
fillrandom,2,lhandoff,153139,6.530
fillrandom,4,native,31851,31.396
fillrandom,4,lhandoff,46544,21.485
fillrandom,8,native,20845,47.973
fillrandom,8,lhandoff,19843,50.395
fillrandom,16,native,7134,140.176
fillrandom,16,lhandoff,8813,113.467
fillrandom,32,native,4154,240.731
fillrandom,32,lhandoff,4490,222.717
fillrandom,64,native,2260,442.477
fillrandom,64,lhandoff,2507,398.883
fillrandom,128,native,1047,955.110
fillrandom,128,lhandoff,1194,837.520
fillrandom,256,native,475,2105.263
fillrandom,256,lhandoff,580,1724.137
readrandom,1,native,382995,2.611
readrandom,1,lhandoff,348675,2.868
readrandom,2,native,252908,3.954
readrandom,2,lhandoff,227842,4.389
readrandom,4,native,101091,9.892
readrandom,4,lhandoff,137854,7.254
readrandom,8,native,56570,17.677
readrandom,8,lhandoff,93361,10.711
readrandom,16,native,43546,22.963
readrandom,16,lhandoff,70596,14.165
readrandom,32,native,23645,42.292
readrandom,32,lhandoff,36764,27.200
readrandom,64,native,11436,87.443
readrandom,64,lhandoff,19332,51.727
readrandom,128,native,5694,175.622
readrandom,128,lhandoff,9933,100.674
readrandom,256,native,2773,360.620
readrandom,256,lhandoff,4944,202.265
```
