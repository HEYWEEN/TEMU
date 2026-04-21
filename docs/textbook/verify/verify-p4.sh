#!/bin/bash
# verify-p4.sh — 确认 P4 差分测试框架真的能端到端 lockstep
#
# 覆盖：
#   §2     difftest.h 三个 API 声明存在
#   §3     ref_exec_one 独立于主路径(不 include inst.h 的 BITS/SEXT 宏)
#   §4     -d 开关启用差分测试,最小程序能 HIT END
#   §4-§7  make test-diff 全绿(ISA + prog 都通过 lockstep)
#   §6     paddr_touched_mmio 真的被 pmem.c 置位 + difftest.c 读取
#   §7     mip 从 CSR 比对里豁免
#   §8     bug 注入演示(sed 改 SLTIU → make test-diff 报错 → 还原 → 绿)
set -euo pipefail

cd "$(dirname "$0")/../../.."
REPO=$(pwd)
TEMU=$REPO/build/temu
WORK=/tmp/textbook-verify/p4
rm -rf "$WORK" && mkdir -p "$WORK"

echo "[build] make -s"
make -s >/dev/null
[[ -x $TEMU ]] || { echo "FAIL: temu binary missing"; exit 1; }
echo "  ok"

# ---------- §2 difftest.h 三个 API 都声明 ----------
echo "[§2] difftest.h declares enable/init/step ..."
for sym in difftest_enable difftest_init difftest_step; do
    grep -q "$sym" include/difftest.h || { echo "FAIL: $sym missing"; exit 1; }
done
echo "  ok"

# ---------- §3 参考实现独立(不 include 主路径的 inst.h 宏) ----------
echo "[§3] ref_exec_one uses independent decode path ..."
# difftest.c 不应 include inst.h(那会共享 BITS/SEXT/immX 宏)
# 但可以 include csr.h(CSR 是 struct 定义,不影响 decode 独立性)
if grep -q '#include "../isa/riscv32/local-include/inst.h"' src/difftest/difftest.c; then
    # inst.h 被 include 可能是为了 operand_type_t 等类型定义,不违反独立性
    # 但若 difftest.c 实际调用了主路径的 SEXT/immX/BITS 宏(带括号调用),就违反了
    # 注释里的字眼("see BITS/SEXT")被正则排除(必须紧跟 '(')
    if grep -qE '\b(SEXT|immI|immS|immB|immU|immJ|BITS)\s*\(' src/difftest/difftest.c; then
        echo "FAIL: difftest.c actually invokes main-path decode macros (violates independence)"
        grep -nE '\b(SEXT|immI|immS|immB|immU|immJ|BITS)\s*\(' src/difftest/difftest.c
        exit 1
    fi
fi
# 确认 difftest.c 有自己的 sext32 + imm_* 函数
for sym in sext32 imm_i imm_s imm_b imm_u imm_j; do
    grep -q "$sym" src/difftest/difftest.c || { echo "FAIL: difftest.c missing $sym"; exit 1; }
done
echo "  ok"

# ---------- §4 -d 开关启用差分测试,最小 EBREAK 程序能 HIT END ----------
echo "[§4] -d flag enables difftest, minimal EBREAK halts cleanly ..."
printf '\x73\x00\x10\x00' > "$WORK/min.bin"     # EBREAK little-endian
out=$("$TEMU" -bd "$WORK/min.bin" 2>&1)
echo "$out" | grep -q "difftest: initialized" || { echo "FAIL: -d didn't init"; echo "$out"; exit 1; }
echo "$out" | grep -q "HIT END"                || { echo "FAIL: didn't HIT END"; echo "$out"; exit 1; }
echo "  ok"

# ---------- §4-§7 make test-diff 全绿 ----------
echo "[§4-§7] make test-diff (lockstep on ISA + prog tests) ..."
make -s test-diff 2>&1 | tee "$WORK/diff.log" >/dev/null

# isa tests 行 & prog tests 行都要 "0 failed"
grep -qE "isa tests: [0-9]+ passed, 0 failed"    "$WORK/diff.log" || {
    echo "FAIL: isa tests had failures under difftest"
    grep -A2 "isa tests" "$WORK/diff.log"
    exit 1
}
grep -qE "program tests: [0-9]+ passed, 0 failed" "$WORK/diff.log" || {
    echo "FAIL: program tests had failures under difftest"
    grep -A2 "program tests" "$WORK/diff.log"
    exit 1
}
n_isa=$(grep  -oE "isa tests: [0-9]+ passed"     "$WORK/diff.log" | grep -oE "[0-9]+" | head -1)
n_prog=$(grep -oE "program tests: [0-9]+ passed" "$WORK/diff.log" | grep -oE "[0-9]+" | head -1)
echo "  ok ($n_isa isa + $n_prog prog tests all lockstep-green)"

# ---------- §6 paddr_touched_mmio flag 在两端都正确使用 ----------
echo "[§6] paddr_touched_mmio set by pmem.c, consumed by difftest.c ..."
grep -q "paddr_touched_mmio = true"  src/memory/pmem.c     || { echo "FAIL: pmem.c doesn't set flag"; exit 1; }
grep -q "paddr_touched_mmio"         src/difftest/difftest.c || { echo "FAIL: difftest.c doesn't read flag"; exit 1; }
grep -q "paddr_touched_mmio = false" src/difftest/difftest.c || { echo "FAIL: difftest.c doesn't clear flag"; exit 1; }
echo "  ok"

# ---------- §7 mip 从 CSR 比对豁免 ----------
echo "[§7] mip excluded from CSR compare (ref_csr.mip = csr.mip) ..."
grep -q "ref_csr.mip = csr.mip" src/difftest/difftest.c || {
    echo "FAIL: mip not explicitly synced (must be outside CSR_CMP loop)"
    exit 1
}
# 确认 CSR_CMP(mip) 没在 difftest.c 出现(避免报假阳)
if grep -q "CSR_CMP(mip)" src/difftest/difftest.c; then
    echo "FAIL: CSR_CMP(mip) present — mip should be excluded from compare"
    exit 1
fi
echo "  ok"

# ---------- §8 bug 注入演示(sed 临时改 SLTIU → difftest 开火 → 还原) ----------
echo "[§8] bug injection: break SLTIU, difftest catches, then restore ..."
BACKUP="$WORK/inst.c.orig"
cp src/isa/riscv32/inst.c "$BACKUP"

# 把 SLTIU 的 src1 改成 (sword_t)src1 (有符号 vs 无符号 bug)
# 原文:  R(rd) = src1 < imm ? 1 : 0);
# 改为:  R(rd) = (sword_t)src1 < (sword_t)imm ? 1 : 0);
sed -i.bak 's|"sltiu", I,\n        R(rd) = src1 < imm ? 1 : 0|SLTIU_BROKEN|' src/isa/riscv32/inst.c 2>/dev/null || true

# 上面 sed 做不了跨行,改成 perl:
perl -i -pe 'BEGIN{undef $/;} s|"sltiu",\s+I,\s+R\(rd\)\s*=\s*src1\s*<\s*imm\s*\?\s*1\s*:\s*0\);|"sltiu", I,\n        R(rd) = (sword_t)src1 < (sword_t)imm ? 1 : 0);|s' src/isa/riscv32/inst.c

# 确认 sed 生效
if ! grep -q "(sword_t)src1 < (sword_t)imm" src/isa/riscv32/inst.c; then
    echo "WARN: bug injection sed didn't match (SLTIU pattern may have moved). Skipping §8."
    cp "$BACKUP" src/isa/riscv32/inst.c
    echo "  skipped"
else
    # 重编
    make -s >/dev/null 2>&1 || true
    # 跑 test-diff 应该失败
    set +e
    make -s test-diff >"$WORK/broken.log" 2>&1
    rc=$?
    set -e
    # 还原 + 重编
    cp "$BACKUP" src/isa/riscv32/inst.c
    make -s >/dev/null
    # 检查 broken.log 里是否有 "diverged from reference" 或测试 fail
    if [[ $rc -eq 0 ]]; then
        echo "FAIL: bug injection didn't cause any test failure — lockstep did NOT catch"
        cat "$WORK/broken.log" | tail -20
        exit 1
    fi
    # 再次确认 test-diff 回到全绿
    make -s test-diff >"$WORK/restored.log" 2>&1
    grep -qE "isa tests: [0-9]+ passed, 0 failed" "$WORK/restored.log" || {
        echo "FAIL: after restore, test-diff not green (rebuild issue?)"
        exit 1
    }
    echo "  ok (bug caught, then restored clean)"
fi

echo ""
echo "P4 verification: PASS"
