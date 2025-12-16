# SPDX-License-Identifier: MIT
# tests/Makefile

CC ?= gcc
CFLAGS := -Wall -Wextra -O2 -g -pthread

TESTS := bench_mutex test_handoff bench_realistic

.PHONY: all clean run

all: $(TESTS)

bench_mutex: bench_mutex.c
	$(CC) $(CFLAGS) $< -o $@

test_handoff: test_handoff.c
	$(CC) $(CFLAGS) $< -o $@

bench_realistic: bench_realistic.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(TESTS)

# 直接运行（不使用 lhandoff）
run-native: $(TESTS)
	@echo "=== bench_mutex (native) ==="
	./bench_mutex
	@echo ""
	@echo "=== test_handoff (native) ==="
	./test_handoff

# 使用 lhandoff 运行
run-lhandoff: $(TESTS)
	@echo "=== bench_mutex (lhandoff) ==="
	sudo ../launcher/lh_launcher ./bench_mutex
	@echo ""
	@echo "=== test_handoff (lhandoff) ==="
	sudo ../launcher/lh_launcher ./test_handoff

# 对比测试
compare: $(TESTS)
	@echo "========== Native =========="
	./bench_mutex
	@echo ""
	@echo "========== With lhandoff =========="
	sudo ../launcher/lh_launcher ./bench_mutex
