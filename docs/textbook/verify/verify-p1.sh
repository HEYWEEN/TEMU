#!/bin/bash
# verify-p1.sh — 确认 P1 章节里讲的所有"调试器能做的事"都真的能做
#
# 不同于 P0（从书里抽代码独立编译），P1 讲的就是 TEMU 的 src/monitor/*。
# 所以验证方式是：在主仓库跑 make + 官方测试，再加一批针对书中具体
# 断言的 REPL / 表达式 smoke case。
#
# 覆盖：
#   §3-5  Lexer + Parser: 优先级、unary、hex、shift、逻辑短路、除零错误
#   §5    parse_shift 的 "mask 31" 行为
#   §6    watchpoint 生命周期（add / list / delete）
#   §7    官方 expr tests + fuzz smoke
set -euo pipefail

cd "$(dirname "$0")/../../.."    # back to repo root
REPO=$(pwd)
TEMU=$REPO/build/temu

echo "[build] ensuring temu is up to date ..."
make -s >/dev/null
[[ -x $TEMU ]] || { echo "FAIL: temu binary missing"; exit 1; }
echo "  ok"

# ---------- §7 golden-file tests ----------
echo "[§7] official expr golden tests ..."
make -s test-expr >/dev/null
echo "  ok"

# ---------- §3-5 handpicked expression cases ----------
echo "[§3-5] precedence / unary / shift / short-circuit / hex / reg ..."
WORK=/tmp/textbook-verify/p1
rm -rf "$WORK" && mkdir -p "$WORK"
cat > "$WORK/p1-cases.txt" << 'EOF'
# precedence
7           1 + 2 * 3
9           (1 + 2) * 3

# unary (4-way disambiguation)
0xfffffffb  -5
0xffffffff  ~0
1           !0
0           !5

# shift with mask-31 behavior (1 << 33 on RV32 == 1 << 1 == 2)
2           1 << 33

# bitwise + logical
0           0 && 1
1           1 || 0
1           0 || 1 && 0 || 1

# hex + derived
0x100       256
0x80        1 << 7

# deref stub (returns 0)
0           *0x80000000

# register stub (isa_reg_val returns 0 for any name)
0           $pc
0           $x0 + $ra
EOF
$TEMU -t "$WORK/p1-cases.txt" 2>&1 | grep -q "failed" && {
    echo "FAIL: handpicked expression cases"
    $TEMU -t "$WORK/p1-cases.txt"
    exit 1
} || true
echo "  ok"

# ---------- §5 error-handling: division by zero doesn't kill host ----------
echo "[§5] division by zero is trapped, not SIGFPE ..."
out=$(printf 'p 1/0\nq\n' | $TEMU 2>&1 || true)
echo "$out" | grep -q "cannot evaluate" || {
    echo "FAIL: '/0' did not produce 'cannot evaluate'"
    echo "--- output ---"; echo "$out"
    exit 1
}
# confirm exit code is 0 (clean exit, not crashed)
set +e
printf 'p 1/0\nq\n' | $TEMU >/dev/null 2>&1
rc=$?
set -e
[[ $rc -eq 0 ]] || { echo "FAIL: TEMU exited non-zero after '1/0' (rc=$rc)"; exit 1; }
echo "  ok"

# ---------- §5 trailing-garbage check ----------
echo "[§5] trailing tokens rejected ..."
out=$(printf 'p 1 2\nq\n' | $TEMU 2>&1 || true)
echo "$out" | grep -qE "(cannot evaluate|trailing garbage)" || {
    echo "FAIL: 'p 1 2' did not produce an error"
    echo "$out"
    exit 1
}
echo "  ok"

# ---------- §6 watchpoint lifecycle ----------
echo "[§6] watchpoint add / list / delete ..."
out=$(printf 'w $pc\nw $x0 + 1\ninfo w\nd 1\ninfo w\nq\n' | $TEMU 2>&1)
echo "$out" | grep -q "watchpoint #1:" || { echo "FAIL: no #1 add"; echo "$out"; exit 1; }
echo "$out" | grep -q "watchpoint #2:" || { echo "FAIL: no #2 add"; echo "$out"; exit 1; }

# Both `info w` segments combined:
#   - row "1    $pc" should appear once (only in the first info-w, before `d 1`)
#   - row "2    $x0 + 1" should appear twice (in both info-w calls)
n1=$(echo "$out" | grep -cE '^1 +\$pc' || true)
n2=$(echo "$out" | grep -cE '^2 +\$x0' || true)
[[ $n1 -eq 1 ]] || { echo "FAIL: row #1 should appear once, saw $n1"; echo "$out"; exit 1; }
[[ $n2 -eq 2 ]] || { echo "FAIL: row #2 should appear twice, saw $n2"; echo "$out"; exit 1; }
echo "  ok"

# ---------- §6 wp_del returns failure for unknown id ----------
echo "[§6] d N reports missing id ..."
out=$(printf 'd 99\nq\n' | $TEMU 2>&1)
echo "$out" | grep -q "no watchpoint #99" || { echo "FAIL: 'd 99' silent"; echo "$out"; exit 1; }
echo "  ok"

# ---------- §7 differential fuzz (short, just confirm it runs) ----------
echo "[§7] fuzz smoke (N=50) ..."
make -s tools >/dev/null
out=$(bash tests/expr/fuzz.sh 50 2>&1)
echo "$out" | tail -2
echo "$out" | grep -q "0 failed" || { echo "FAIL: fuzz produced failures"; echo "$out"; exit 1; }
echo "  ok"

echo ""
echo "P1 verification: PASS"
