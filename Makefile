# SPDX-License-Identifier: MIT
# lhandoff - Makefile

CLANG ?= clang
LLC ?= llc
CC ?= gcc
BPFTOOL ?= bpftool

# 目录
COMMON_DIR := common
SCX_DIR := scx
LIBLH_DIR := liblh
LAUNCHER_DIR := launcher

# 编译选项
CFLAGS := -Wall -Wextra -O2 -g
LDFLAGS := -lpthread -ldl

BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_x86

# libbpf
LIBBPF_CFLAGS := $(shell pkg-config --cflags libbpf 2>/dev/null || echo "-I/usr/include")
LIBBPF_LDFLAGS := $(shell pkg-config --libs libbpf 2>/dev/null || echo "-lbpf -lelf -lz")

# 输出
BPF_OBJ := $(SCX_DIR)/scx_lhandoff.bpf.o
BPF_SKEL := $(LAUNCHER_DIR)/scx_lhandoff.skel.h
LIBLH_SO := $(LIBLH_DIR)/liblh.so
LAUNCHER := $(LAUNCHER_DIR)/lh_launcher

# vmlinux.h 路径
VMLINUX_H := $(SCX_DIR)/vmlinux.h

.PHONY: all clean vmlinux

all: $(VMLINUX_H) $(BPF_SKEL) $(LIBLH_SO) $(LAUNCHER)

# 生成 vmlinux.h
vmlinux: $(VMLINUX_H)

$(VMLINUX_H):
	@echo "Generating vmlinux.h..."
	@mkdir -p $(SCX_DIR)
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

# 编译 BPF 程序
$(BPF_OBJ): $(SCX_DIR)/scx_lhandoff.bpf.c $(COMMON_DIR)/lh_shared.h $(VMLINUX_H)
	@echo "Compiling BPF program..."
	$(CLANG) $(BPF_CFLAGS) -I$(SCX_DIR) -c $< -o $@

# 生成 BPF skeleton
$(BPF_SKEL): $(BPF_OBJ)
	@echo "Generating BPF skeleton..."
	$(BPFTOOL) gen skeleton $< > $@

# 编译 liblh.so
$(LIBLH_SO): $(LIBLH_DIR)/liblh.c $(COMMON_DIR)/lh_shared.h
	@echo "Compiling liblh.so..."
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@ $(LDFLAGS)

# 编译 launcher (不再依赖 skeleton)
$(LAUNCHER): $(LAUNCHER_DIR)/lh_launcher.c $(BPF_OBJ) $(COMMON_DIR)/lh_shared.h
	@echo "Compiling launcher..."
	$(CC) $(CFLAGS) $(LIBBPF_CFLAGS) $< -o $@ $(LIBBPF_LDFLAGS)

clean:
	rm -f $(BPF_OBJ) $(BPF_SKEL) $(LIBLH_SO) $(LAUNCHER)
	rm -f $(VMLINUX_H)

# 安装
install: all
	install -m 755 $(LAUNCHER) /usr/local/bin/lh_launcher
	install -m 755 $(LIBLH_SO) /usr/local/lib/liblh.so
	ldconfig

# 测试
test: all
	@echo "Running basic test..."
	sudo $(LAUNCHER) /bin/ls -la

.PHONY: install test
