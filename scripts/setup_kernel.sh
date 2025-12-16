#!/bin/bash
# SPDX-License-Identifier: MIT
# setup_kernel.sh - 检查和配置内核 sched_ext 支持

set -e

echo "=== lhandoff Kernel Setup ==="

# 检查内核版本
KERNEL_VERSION=$(uname -r)
echo "Kernel version: $KERNEL_VERSION"

# 检查 sched_ext 支持
check_scx_support() {
    if [ -d "/sys/kernel/sched_ext" ]; then
        echo "✓ sched_ext support detected"
        return 0
    fi
    
    # 检查内核配置
    if [ -f "/boot/config-$KERNEL_VERSION" ]; then
        if grep -q "CONFIG_SCHED_CLASS_EXT=y" "/boot/config-$KERNEL_VERSION"; then
            echo "✓ CONFIG_SCHED_CLASS_EXT enabled in kernel config"
            return 0
        fi
    fi
    
    echo "✗ sched_ext not detected"
    echo ""
    echo "To enable sched_ext, you need:"
    echo "1. Linux kernel >= 6.12 with CONFIG_SCHED_CLASS_EXT=y"
    echo "2. Or use a kernel with sched_ext backport"
    echo ""
    echo "Recommended: Use the sched_ext development kernel from:"
    echo "  https://github.com/sched-ext/scx"
    return 1
}

# 检查 BTF 支持
check_btf_support() {
    if [ -f "/sys/kernel/btf/vmlinux" ]; then
        echo "✓ BTF support detected"
        return 0
    fi
    echo "✗ BTF not available (needed for BPF CO-RE)"
    return 1
}

# 检查 BPF 权限
check_bpf_permissions() {
    if [ "$(id -u)" -eq 0 ]; then
        echo "✓ Running as root"
        return 0
    fi
    
    # 检查 CAP_BPF
    if capsh --print 2>/dev/null | grep -q "cap_bpf"; then
        echo "✓ CAP_BPF capability available"
        return 0
    fi
    
    echo "⚠ Not running as root, may need sudo for BPF operations"
    return 0
}

# 检查依赖
check_dependencies() {
    echo ""
    echo "Checking dependencies..."
    
    local missing=()
    
    command -v clang >/dev/null || missing+=("clang")
    command -v bpftool >/dev/null || missing+=("bpftool")
    pkg-config --exists libbpf 2>/dev/null || missing+=("libbpf-dev")
    
    if [ ${#missing[@]} -eq 0 ]; then
        echo "✓ All dependencies installed"
        return 0
    fi
    
    echo "✗ Missing dependencies: ${missing[*]}"
    echo ""
    echo "Install with:"
    echo "  Ubuntu/Debian: sudo apt install clang llvm libbpf-dev linux-tools-common"
    echo "  Fedora: sudo dnf install clang llvm libbpf-devel bpftool"
    return 1
}

# 主检查
main() {
    local errors=0
    
    check_scx_support || ((errors++))
    check_btf_support || ((errors++))
    check_bpf_permissions
    check_dependencies || ((errors++))
    
    echo ""
    if [ $errors -eq 0 ]; then
        echo "=== All checks passed! Ready to build. ==="
    else
        echo "=== $errors check(s) failed. Please fix before building. ==="
        exit 1
    fi
}

main "$@"
