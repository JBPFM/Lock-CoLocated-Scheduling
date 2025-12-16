#!/bin/bash
# LevelDB db_bench 性能测试脚本

set -e

DB_BENCH="/home/jz/test/test/leveldb/build/db_bench"
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LAUNCHER="$SCRIPT_DIR/launcher/lh_launcher"
LIBLH="$SCRIPT_DIR/liblh/liblh.so"
BPF_OBJ="$SCRIPT_DIR/scx/scx_lhandoff.bpf.o"
NUM=100000
DB_DIR="/tmp/leveldb_bench"

THREADS=(1 2 4 8 16 32 64 128 256)

# 输出目录
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_DIR="$SCRIPT_DIR/results/leveldb_$TIMESTAMP"
mkdir -p "$OUTPUT_DIR"

RESULT_FILE="$OUTPUT_DIR/results.csv"
LOG_DIR="$OUTPUT_DIR/logs"
mkdir -p "$LOG_DIR"

echo "=== LevelDB db_bench Performance Test ==="
echo "DB_BENCH: $DB_BENCH"
echo "LAUNCHER: $LAUNCHER"
echo "NUM: $NUM"
echo "OUTPUT_DIR: $OUTPUT_DIR"
echo ""

# 保存测试配置
cat > "$OUTPUT_DIR/config.txt" << EOF
Test Configuration
==================
Date: $(date)
Kernel: $(uname -r)
CPU: $(grep "model name" /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)
CPU Cores: $(nproc)
DB_BENCH: $DB_BENCH
NUM: $NUM
THREADS: ${THREADS[*]}
EOF

if [ ! -x "$DB_BENCH" ]; then
    echo "Error: db_bench not found at $DB_BENCH"
    exit 1
fi

echo "benchmark,threads,mode,ops_per_sec,micros_per_op" > $RESULT_FILE

extract_result() {
    local output="$1"
    local benchmark="$2"

    local line=$(echo "$output" | grep -E "^$benchmark\s*:")
    if [ -z "$line" ]; then
        echo "N/A N/A"
        return
    fi

    local micros=$(echo "$line" | awk '{print $3}')
    if [ -n "$micros" ] && [ "$micros" != "N/A" ]; then
        local ops=$(echo "scale=0; 1000000 / $micros" | bc 2>/dev/null || echo "N/A")
        echo "$ops $micros"
    else
        echo "N/A N/A"
    fi
}

cleanup_db() {
    sudo rm -rf $DB_DIR 2>/dev/null || true
    mkdir -p $DB_DIR
    chmod 777 $DB_DIR
}

run_native() {
    local benchmark=$1
    local threads=$2
    local extra_args=$3
    local log_file="$LOG_DIR/${benchmark}_t${threads}_native.log"

    cleanup_db

    local output=$($DB_BENCH --benchmarks=$benchmark --threads=$threads --num=$NUM --db=$DB_DIR $extra_args 2>&1)
    echo "$output" > "$log_file"
    extract_result "$output" "$benchmark"
}

run_lhandoff() {
    local benchmark=$1
    local threads=$2
    local extra_args=$3
    local log_file="$LOG_DIR/${benchmark}_t${threads}_lhandoff.log"

    cleanup_db

    local output=$(sudo $LAUNCHER -b $BPF_OBJ -l $LIBLH $DB_BENCH --benchmarks=$benchmark --threads=$threads --num=$NUM --db=$DB_DIR $extra_args 2>&1)
    echo "$output" > "$log_file"
    extract_result "$output" "$benchmark"
}

echo "========== fillrandom Tests =========="
for t in "${THREADS[@]}"; do
    echo -n "Threads $t: "

    result=$(run_native "fillrandom" $t "")
    ops_n=$(echo $result | awk '{print $1}')
    us_n=$(echo $result | awk '{print $2}')
    echo -n "native=$ops_n ops/s, "
    echo "fillrandom,$t,native,$ops_n,$us_n" >> $RESULT_FILE

    result=$(run_lhandoff "fillrandom" $t "")
    ops_l=$(echo $result | awk '{print $1}')
    us_l=$(echo $result | awk '{print $2}')
    echo "lhandoff=$ops_l ops/s"
    echo "fillrandom,$t,lhandoff,$ops_l,$us_l" >> $RESULT_FILE
done

echo ""
echo "========== readrandom Tests =========="
echo "Preparing database..."
cleanup_db
$DB_BENCH --benchmarks=fillseq --threads=1 --num=$((NUM * 10)) --db=$DB_DIR > "$LOG_DIR/fillseq_prepare.log" 2>&1

for t in "${THREADS[@]}"; do
    echo -n "Threads $t: "

    output=$($DB_BENCH --benchmarks=readrandom --threads=$t --num=$NUM --db=$DB_DIR --use_existing_db=1 2>&1)
    echo "$output" > "$LOG_DIR/readrandom_t${t}_native.log"
    result=$(extract_result "$output" "readrandom")
    ops_n=$(echo $result | awk '{print $1}')
    us_n=$(echo $result | awk '{print $2}')
    echo -n "native=$ops_n ops/s, "
    echo "readrandom,$t,native,$ops_n,$us_n" >> $RESULT_FILE

    output=$(sudo $LAUNCHER -b $BPF_OBJ -l $LIBLH $DB_BENCH --benchmarks=readrandom --threads=$t --num=$NUM --db=$DB_DIR --use_existing_db=1 2>&1)
    echo "$output" > "$LOG_DIR/readrandom_t${t}_lhandoff.log"
    result=$(extract_result "$output" "readrandom")
    ops_l=$(echo $result | awk '{print $1}')
    us_l=$(echo $result | awk '{print $2}')
    echo "lhandoff=$ops_l ops/s"
    echo "readrandom,$t,lhandoff,$ops_l,$us_l" >> $RESULT_FILE
done

sudo rm -rf $DB_DIR 2>/dev/null || true

echo ""
echo "========== Results Summary =========="
echo ""

# 生成摘要文件
SUMMARY_FILE="$OUTPUT_DIR/summary.txt"
{
    printf "%-12s %8s %14s %14s %14s %14s %8s\n" "Benchmark" "Threads" "Native(ops/s)" "lhandoff(ops/s)" "Native(us/op)" "lhandoff(us/op)" "Speedup"
    printf "%-12s %8s %14s %14s %14s %14s %8s\n" "-----------" "-------" "--------------" "---------------" "--------------" "---------------" "-------"

    for bench in fillrandom readrandom; do
        for t in "${THREADS[@]}"; do
            native_ops=$(grep "^$bench,$t,native" $RESULT_FILE | cut -d, -f4)
            native_us=$(grep "^$bench,$t,native" $RESULT_FILE | cut -d, -f5)
            lhandoff_ops=$(grep "^$bench,$t,lhandoff" $RESULT_FILE | cut -d, -f4)
            lhandoff_us=$(grep "^$bench,$t,lhandoff" $RESULT_FILE | cut -d, -f5)

            if [ "$native_ops" != "N/A" ] && [ "$lhandoff_ops" != "N/A" ] && [ -n "$native_ops" ] && [ -n "$lhandoff_ops" ]; then
                speedup=$(echo "scale=2; $lhandoff_ops / $native_ops" | bc 2>/dev/null || echo "N/A")
                printf "%-12s %8d %14s %15s %14s %15s %7sx\n" "$bench" "$t" "$native_ops" "$lhandoff_ops" "$native_us" "$lhandoff_us" "$speedup"
            else
                printf "%-12s %8d %14s %15s %14s %15s %8s\n" "$bench" "$t" "$native_ops" "$lhandoff_ops" "$native_us" "$lhandoff_us" "N/A"
            fi
        done
    done
} | tee "$SUMMARY_FILE"

echo ""
echo "========== Output Files =========="
echo "Results CSV: $RESULT_FILE"
echo "Summary: $SUMMARY_FILE"
echo "Logs directory: $LOG_DIR"
echo "Config: $OUTPUT_DIR/config.txt"
echo ""
echo "All files saved to: $OUTPUT_DIR"

# 创建符号链接到最新结果
ln -sfn "$OUTPUT_DIR" "$SCRIPT_DIR/results/latest"
echo "Latest results linked to: $SCRIPT_DIR/results/latest"
