# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
# mutex_probe Makefile — 支持 libbpf v1.6 符号名挂载

### 工具定义 ###
BPF_CLANG ?= clang
BPF_LLVM_STRIP ?= llvm-strip
CC := gcc

### 路径配置 ###
LIBBPF_SRC_DIR := /home/sched_ext/libbpf/src
LIBBPF_INC_DIR := /home/sched_ext/libbpf/src
BPFTOOL := /home/sched_ext/linux-6.15.5/tools/bpf/bpftool/bpftool
KERN_TOOLS := /home/sched_ext/linux-6.15.5/tools/include
KERN_UAPI := /home/sched_ext/linux-6.15.5/tools/include/uapi

### eBPF 编译标志 ###
BPF_CFLAGS := -I$(LIBBPF_INC_DIR) -I. \
              -I$(KERN_TOOLS) -I$(KERN_UAPI) \
              -Wall -g

### 用户态编译标志 ###
USER_CFLAGS := -I$(LIBBPF_INC_DIR) -I. -Wall -g
USER_LDFLAGS := -L$(LIBBPF_SRC_DIR) -lbpf -lelf -lz -pthread -ldl

### 文件定义 ###
BPF_SRC := mutex_probe.bpf.c
BPF_OBJ := mutex_probe.bpf.o
SKEL_HDR := mutex_probe.skel.h

USER_SRC := mutex_probe_user.c
USER_BIN := mutex_probe

### 默认目标 ###
all: $(USER_BIN)

### Step 1: 编译 eBPF 程序 ###
$(BPF_OBJ): $(BPF_SRC)
	$(BPF_CLANG) -O2 -g -target bpf -D__TARGET_ARCH_x86 $(BPF_CFLAGS) -c $< -o $@
	$(BPF_LLVM_STRIP) -g $@

### Step 2: 生成 skeleton 头文件 ###
$(SKEL_HDR): $(BPF_OBJ)
	@echo "[INFO] Generating skeleton header..."
	$(BPFTOOL) gen skeleton $< > $@
	@echo "[INFO] (Optional) Regenerating vmlinux.h from kernel BTF..."
	@$(BPFTOOL) btf dump file /home/sched_ext/linux-6.15.5/vmlinux format c > vmlinux.h || true

### Step 3: 编译用户态程序 ###
$(USER_BIN): $(USER_SRC) $(SKEL_HDR)
	$(CC) $(USER_CFLAGS) -o $@ $(USER_SRC) $(USER_LDFLAGS)

### Step 4: 清理 ###
clean:
	rm -f $(BPF_OBJ) $(SKEL_HDR) $(USER_BIN)

.PHONY: all clean
