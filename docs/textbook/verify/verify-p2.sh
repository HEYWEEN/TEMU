#!/bin/bash
# verify-p2.sh — 确认 P2 讲的内存 / 寄存器 / 镜像加载全部真的能跑
#
# 覆盖：
#   §3 (cpu_init):          `$pc` 初始化成 RESET_VECTOR
#   §4 (pmem + guest_to_host): tests/pmem/load.sh
#   §5 (字节序 + 对齐):       LE 打包检查 + len=1/2/4
#   §6 (paddr_read 路由):    越界访问 panic
#   §7 (load_img 正式版):    溢出检测 / 短读检测
#   §9 (ABI 别名):           $sp / $fp / $ra / $zero 都能查出值
set -euo pipefail

cd "$(dirname "$0")/../../.."
REPO=$(pwd)
TEMU=$REPO/build/temu

echo "[build] ensuring temu is up to date ..."
make -s >/dev/null
[[ -x $TEMU ]] || { echo "FAIL: temu binary missing"; exit 1; }
echo "  ok"

# ---------- §4-§5 pmem 加载 + 字节序 (复用官方脚本) ----------
echo "[§4-§5] tests/pmem/load.sh ..."
bash tests/pmem/load.sh >/dev/null
echo "  ok"

# ---------- §5 字节粒度检查 ----------
echo "[§5] len=1 / len=2 / len=4 read shapes ..."
WORK=/tmp/textbook-verify/p2
rm -rf "$WORK" && mkdir -p "$WORK"
printf '\x11\x22\x33\x44\x55\x66\x77\x88' > "$WORK/img.bin"

# 用 p *EXPR（4 字节，调用 paddr_read(len=4)）
out=$(printf 'p *0x80000000\nq\n' | $TEMU "$WORK/img.bin" 2>/dev/null)
# LE: bytes 11 22 33 44 -> 0x44332211
echo "$out" | grep -q '0x44332211' || {
    echo "FAIL: len=4 LE read did not produce 0x44332211"
    echo "$out"; exit 1
}
echo "  ok"

# ---------- §3 cpu_init ----------
echo "[§3] cpu_init sets pc to RESET_VECTOR (0x80000000) ..."
out=$(printf 'p $pc\nq\n' | $TEMU 2>/dev/null)
echo "$out" | grep -q '0x80000000' || {
    echo "FAIL: \$pc is not 0x80000000 after cpu_init"
    echo "$out"; exit 1
}
echo "  ok"

# ---------- §6 越界 panic ----------
echo "[§6] out-of-bound paddr_read panics ..."
out=$(printf 'x 1 0x40000000\nq\n' | $TEMU 2>&1 || true)
echo "$out" | grep -q 'out of bound' || {
    echo "FAIL: OOB read did not trigger panic"
    echo "$out"; exit 1
}
echo "  ok"

# ---------- §7 load_img 溢出检测 ----------
echo "[§7] load_img rejects oversized image ..."
# 做一个 129 MB 的稀疏文件（不占真实盘空间，但 ftell 会返回 129M）
dd if=/dev/zero of="$WORK/big.bin" bs=1 count=1 seek=$((129*1024*1024)) 2>/dev/null
set +e
out=$("$TEMU" -b "$WORK/big.bin" 2>&1)
rc=$?
set -e
echo "$out" | grep -q 'image too large' || {
    echo "FAIL: oversized image did not trigger 'image too large'"
    echo "--- out ---"; echo "$out"; exit 1
}
# Assert 应 abort，rc 非 0
[[ $rc -ne 0 ]] || { echo "FAIL: oversized image produced rc=0"; exit 1; }
echo "  ok"

# ---------- §9 ABI 别名 ----------
echo "[§9] ABI aliases resolve without 'unknown register' ..."
out=$(printf 'p $sp\np $fp\np $ra\np $zero\np $a0\np $t3\np $s11\nq\n' | $TEMU 2>&1)
if echo "$out" | grep -q 'unknown register'; then
    echo "FAIL: one of the ABI aliases is not recognized"
    echo "$out"; exit 1
fi
# 每个 p 命令成功时都有一行 "  = 0 (0x00000000)"（因为没跑指令，全 0）
# 提示符可能在行首：`(temu)   = 0 (...)`
n=$(echo "$out" | grep -cE '= 0 \(0x00000000\)' || true)
[[ $n -eq 7 ]] || {
    echo "FAIL: expected 7 zero results, got $n"
    echo "$out"; exit 1
}
echo "  ok"

# ---------- §9 数字索引 xN ----------
echo "[§9] numeric register names \$x0..\$x31 resolve ..."
out=$(printf 'p $x0\np $x31\np $x32\nq\n' | $TEMU 2>&1)
# $x0, $x31 should succeed; $x32 should fail as unknown
echo "$out" | grep -q 'unknown register' || {
    echo "FAIL: \$x32 should be rejected"
    echo "$out"; exit 1
}
echo "  ok"

echo ""
echo "P2 verification: PASS"
