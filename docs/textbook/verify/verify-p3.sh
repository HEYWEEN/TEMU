#!/bin/bash
# verify-p3.sh — 确认 P3 的 37 条 RV32I 指令 + 主循环 + itrace 全部真的能跑
#
# 覆盖：
#   §3     fetch-decode-execute 循环（最小 EBREAK 程序能清退）
#   §4-§7  全部 37 条指令（走 make test-isa）
#   §7.2   x0 硬连线、符号扩展、JAL link、分支方向
#   §8     x0 硬连线（命名 test 被跑到）
#   §9     itrace 在 abort 时 dump
#   §10    fib / sum / recursive fib 真程序
#   跨章   P1 watchpoint 第一次真的触发（P3 让 CPU 动起来）
set -euo pipefail

cd "$(dirname "$0")/../../.."
REPO=$(pwd)
TEMU=$REPO/build/temu
WORK=/tmp/textbook-verify/p3
rm -rf "$WORK" && mkdir -p "$WORK"

echo "[build] ensuring temu is up to date ..."
make -s >/dev/null
[[ -x $TEMU ]] || { echo "FAIL: temu binary missing"; exit 1; }
echo "  ok"

# ---------- §3 最小 EBREAK 程序能 HIT END ----------
echo "[§3] minimal EBREAK program halts cleanly ..."
# EBREAK = 0x00100073, little-endian = 73 00 10 00
printf '\x73\x00\x10\x00' > "$WORK/min.bin"
out=$("$TEMU" -b "$WORK/min.bin" 2>&1)
echo "$out" | grep -q "HIT END" || {
    echo "FAIL: minimal program did not HIT END"
    echo "$out"; exit 1
}
echo "  ok"

# ---------- §4-§7 全部 isa 测试通过 ----------
echo "[§4-§7] make test-isa (all RV32I instructions + Zicsr + trap) ..."
# 直接跑 ISA test runner + ISA_VERBOSE=1,让所有 case 都打名,spot-check 可 grep
ISA_VERBOSE=1 "$REPO/build/tests/isa/isa" "$TEMU" 2>&1 | tee "$WORK/isa.log" >/dev/null
tail -1 "$WORK/isa.log" | grep -q "0 failed" || {
    echo "FAIL: isa tests had failures"
    tail -20 "$WORK/isa.log"
    exit 1
}
n_pass=$(tail -1 "$WORK/isa.log" | sed -n 's/.*isa tests: \([0-9]*\) passed.*/\1/p')
echo "  ok ($n_pass tests passed)"

# ---------- §7.2 具体 spot-checks 从 isa 日志拿 ----------
for name in "x0 read is zero" "sb + lb sign ext" "sb + lbu zero ext" \
            "jal link + jump" "beq taken" "bne loop countdown" \
            "sltiu ne-max" "srai neg" "sra neg"; do
    if ! grep -q "$name" "$WORK/isa.log"; then
        echo "FAIL: spot-check test '$name' did not run"
        exit 1
    fi
done
echo "[§7.2] x0 / SEXT / JAL link / branch / sltiu / SRA spot-checks present ..."
echo "  ok"

# ---------- §9 itrace dump 在 invalid instruction abort 时触发 ----------
echo "[§9] itrace dumps on invalid instruction ..."
printf '\xff\xff\xff\xff' > "$WORK/bad.bin"
out=$("$TEMU" -b "$WORK/bad.bin" 2>&1 || true)
echo "$out" | grep -q "HIT ABORT"              || { echo "FAIL: no HIT ABORT"; echo "$out"; exit 1; }
echo "$out" | grep -q "itrace (last"           || { echo "FAIL: no itrace dump"; echo "$out"; exit 1; }
echo "$out" | grep -q "0x80000000"             || { echo "FAIL: itrace missing pc 0x80000000"; echo "$out"; exit 1; }
echo "  ok"

# ---------- §10 真程序 fib / sum / recursive fib ----------
echo "[§10] make test-prog (fib / sum / recursive fib) ..."
make -s test-prog 2>&1 | tee "$WORK/prog.log" >/dev/null
# prog runner 是 "silent on pass, loud on fail", grep "passed, 0 failed"
tail -1 "$WORK/prog.log" | grep -qE "program tests: [0-9]+ passed, 0 failed" || {
    echo "FAIL: prog tests had failures"
    tail -20 "$WORK/prog.log"
    exit 1
}
n_pass=$(tail -1 "$WORK/prog.log" | sed -n 's/program tests: \([0-9]*\) passed.*/\1/p')
[[ "$n_pass" -ge 5 ]] || { echo "FAIL: expected ≥5 prog tests, got $n_pass"; exit 1; }
echo "  ok ($n_pass tests passed)"

# ---------- 跨章：P1 watchpoint 第一次真的触发 ----------
echo "[P1→P3] watchpoint fires when CPU runs ADDI ..."
# Program: ADDI a0, zero, 5; EBREAK
# ADDI(A0, ZERO, 5) = 0x00500513; EBREAK = 0x00100073 (all little-endian in file)
printf '\x13\x05\x50\x00\x73\x00\x10\x00' > "$WORK/wp.bin"
out=$(printf 'w $a0\nc\nq\n' | "$TEMU" "$WORK/wp.bin" 2>&1)
echo "$out" | grep -q "watchpoint #1" || {
    echo "FAIL: watchpoint didn't register"; echo "$out"; exit 1
}
echo "$out" | grep -q "changed" || {
    echo "FAIL: watchpoint didn't fire on a0 change"; echo "$out"; exit 1
}
echo "  ok"

echo ""
echo "P3 verification: PASS"
