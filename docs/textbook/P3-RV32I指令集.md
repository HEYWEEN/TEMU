---
title: "P3 · RV32I 指令集：让 CPU 真正开始执行"
date: 2026-06-12
categories: [计算机系统]
tags: [RV32I, ISA, 指令编码, 译码, INSTPAT, 分支, 跳转, RISC, 冯·诺依曼]
math: true
---

## 本章目标

P2 给了这台机器**状态**（寄存器 + PC + 内存）。P3 给它**规则**——RV32I 指令集。

读完本章你能回答：

- 为什么 RV32I 只有 37 条指令，却"什么都能算"？
- 指令格式的六种分类（R/I/S/B/U/J）不是编号游戏，它的依据是什么？
- B-type 立即数在指令字里为什么是"乱的"？
- `cpu.pc += 4` 为什么是错的——至少是不完整的？
- 一条 `jal ra, 0x100` 执行完，CPU 的哪些状态被改了？恰好几个？
- 我们的 `INSTPAT` 模式匹配和真硬件的 PLA 到底有什么关系？

**动手做到**：

- 实现一个可分派 37 条指令的 `INSTPAT` 驱动译码器
- 手写汇编的 fib(10)，在自己的模拟器上跑出 55
- 用 `si 1` 单步看 `$pc` / `$ra` / `$sp` 的变化
- 让 P1 的 watchpoint **第一次真的触发**

**本章不涉及**（defer 清单）：

- **CSR / ECALL / MRET** → P6a
- **压缩指令 C 扩展 / M / A / F / D / V 扩展** → 只提存在，不实现
- **分支预测 / 流水线 / 超标量** → 仅作历史提及
- **特权模式 / MMU** → P6
- **差分测试** → P4 会拿 `isa_exec_once` 当对拍单位

---

## 1. 问题动机：stub 的最后一块多米诺

翻开 P2 结尾，`exec_once` 还是占位：

```c
/* 今天的 exec_once 是这样 —— stage 3 之前的版本 */
static void exec_once(void) {
    /* stub: will be filled in P3 */
}
```

`cpu_exec` 循环每次叫它，它什么也不做。P1 的 `si 10` 不会真的前进 10 条指令，P1 的 watchpoint 永远不触发，batch 模式转着空圈直到被 Ctrl+C。

**这一章的多米诺**：把这一个函数填实，所有之前搭好的脚手架同时上线。

| P1/P2 的 stub | P3 兑现 |
|---|---|
| `si N` 不前进 | 真的执行 N 条 |
| watchpoint 永不触发 | 指令改了被监视值，立刻停 |
| `itrace` 无数据 | 最近 16 条指令的汇编都记下 |
| batch 模式转死循环 | 程序跑到 `ebreak` 干净退出 |
| `isa.c` 测试全挂 | 每条指令一个单元测试全绿 |

整章可以压成一句口号：

> **核心洞察**：CPU = 状态 + 迁移规则。P2 给了状态，P3 给规则。**你已经写过一条指令了**——P0 toy CPU 的那条 ADDI。这章就是把那个循环做对 37 遍，再把编码精度从 `op = inst >> 24` 升级成真 ISA 的精度。

---

## 2. 核心概念：ISA 是契约

### 2.1 ISA = 状态 + 规则 + 编码

一句话定义 ISA（Instruction Set Architecture）：

```
ISA = 一组状态 (gpr + pc + mem + csr + ...)
    + 一组迁移规则 (指令 → 状态变化)
    + 它们的二进制编码 (如何把规则写进字节)
```

这三件事**必须一起定义**才算一个 ISA。RV32I spec 前 80 页就是这三件事的正式定义。

### 2.2 理论视角：ISA 是契约，不是实现

ISA 不是"某款芯片能做的事"。它是**硬件和软件之间长达几十年的合同**：

- 今天写的 x86-64 二进制——**还能在 1978 年的 Intel 8086 上跑一部分**（通过 16-bit compatibility mode）
- Intel Skylake 能运行 80386 的代码、80286 的代码、8086 的代码——每剥一层"像一台时间机器"，而所有这些剥层都由同一份 ISA 合同保证

对我们写模拟器来说，这意味着一件关键事：

> **工程原则**：我们实现的是 **spec**，不是某款芯片。当 spec 和 `gcc` 产生歧义（比如 `SRAI` 在 C 里是 implementation-defined behavior），我们按 spec 选，不按 gcc 选。当 gcc 产生的程序跑不对，bug 要么在 gcc、要么在我们理解 spec 错了——不是 "spec 没说清"。

选 RV32I 的另一个务实原因：**spec 能一次读完**。unprivileged 80 页，privileged 另外 140 页。x86 spec 是几千页分三大卷，手搓一个模拟器是一辈子的事。

---

## 3. 核心循环：Fetch → Decode → Execute

在实现任何指令**之前**，先把循环架起来。

### 3.1 Decode 结构体：一条指令的"脚手架"

```c
// include/cpu.h
typedef struct {
    vaddr_t  pc;    /* address of this instruction                  */
    vaddr_t  snpc;  /* static next PC  (= pc + 4)  — 顺序 fall-through */
    vaddr_t  dnpc;  /* dynamic next PC — what cpu_exec writes back   */
    uint32_t inst;  /* 32-bit encoded instruction                   */
} Decode;
```

三个 PC 相关字段各有分工：

- **`pc`**——这条指令的地址，用于 PC 相对寻址（`auipc`、分支/跳转目标计算）
- **`snpc` static next**——`pc + 4`，顺序下一条指令的地址
- **`dnpc` dynamic next**——真正要写回到 `cpu.pc` 的值。默认等于 `snpc`，指令只在**想跳转时才覆盖**它

这个"三字段设计"是 P3 最重要的架构决策。

### 3.2 为什么不直接 `cpu.pc += 4`

初学者的本能写法：

```c
/* v1 朴素版 —— 不要这样写 */
static void exec_once_v1(void) {
    uint32_t inst = paddr_read(cpu.pc, 4);
    switch (opcode) {
        case ADDI: /* ... */    cpu.pc += 4; break;
        case BEQ: if (cond) cpu.pc += offset; else cpu.pc += 4; break;
        case JAL: cpu.pc = target; break;
        /* ... 每一条都要显式写 pc 更新 */
    }
}
```

问题：

- **每条指令都要记得写 `cpu.pc += 4`**——漏一条就死循环
- **分支指令要同时处理 "taken" 和 "not taken"**——两个 `cpu.pc = ...`
- **跳转覆盖别的 PC**——如果你先 `cpu.pc += 4` 再 `cpu.pc = target`，会多做一次加法；顺序错就是 bug

**v2 正式版**：PC 更新的权力**属于循环，不属于指令**。每条指令只写 `s->dnpc`：

```c
// src/cpu/cpu_exec.c 的 exec_once 真实版 (暂去掉 itrace/trap/difftest)
static void exec_once(void) {
    Decode s;
    s.pc   = cpu.pc;
    s.snpc = cpu.pc + 4;
    s.dnpc = s.snpc;                /* 默认顺序 fall-through */
    s.inst = (uint32_t)paddr_read(s.pc, 4);

    isa_exec_once(&s);              /* 指令体只写 s.dnpc（可能） */

    if (g.state == TEMU_RUNNING) {
        cpu.pc = s.dnpc;            /* 循环接管 PC 写回 */
    }
    cpu.gpr[0] = 0;                 /* x0 硬连线:见 §8 */
}
```

现在每条指令的职责变成：

- **算术指令**（`addi` / `add` ...）：写 `R(rd)`，**不碰 `s->dnpc`**（默认顺序继续）
- **分支指令**（`beq` ...）：满足条件时 `s->dnpc = s->pc + offset`
- **跳转指令**（`jal` / `jalr`）：无条件 `s->dnpc = target`，同时写 `R(rd) = s->snpc`（链接地址）

指令**不可能忘记 PC 更新**——默认就是对的。

### 3.2.1 理论视角：PC 更新的三段历史教训

"PC 的更新应该由谁负责"看似细节，实际是 RISC 设计史上反复被咬的一块肉。TEMU 的三字段 `{pc, snpc, dnpc}` 设计不是凭直觉——它是 **ARM R15 陷阱** 和 **MIPS 分支延迟槽** 两代教训的总结。

**教训一：ARM R15 的 "PC+8" 陷阱（1985）**

经典 ARM（A32）把 PC 当成 `R15` 放进 GPR。后果：

```
MOV R0, R15         ; 读 PC,但读到的是什么?
```

答案让无数新手崩溃：由于早期 ARM 是**三级流水**（取指 → 译码 → 执行），执行阶段读 R15 时，**取指已经走到 PC + 8**——所以 `R0 = PC + 8`，不是当前指令的 PC。这让每本 ARM 汇编教材都要用半页讲"+8 调整"。

更糟的是**任何写 R15 的指令都是跳转**。`ADD R15, R15, R0` 是个合法的 ADD，语义上等于"跳到 `PC + R0`"——分支预测器必须把所有"写 R15"的指令当潜在跳转看，硬件复杂度爆炸。

**ARMv8 AArch64（2011）彻底改道**：PC 从 GPR 拆出来，只有 B / BR / RET 能改 PC。这正是 RV32I 从一开始就做的事。

**教训二：MIPS 的 "分支延迟槽"（1985）**

MIPS R2000 当年为了简化流水线，让分支指令**后面一条**总是被执行——哪怕分支成立：

```asm
beq  $t0, $t1, label    ; 分支指令
addi $a0, $a0, 1        ; 延迟槽:无论分支是否成立,这条必跑
label:
```

`addi` 在"分支槽"里——设计者想让编译器把有用指令塞进去。但这造成：

- **汇编反直觉**：跳转"不是立刻跳"
- **编译器负担**：得找指令填槽，填不到就只能填 `nop`
- **异常语义扭曲**：如果槽里的指令 trap 了，异常处理器要保存**分支指令的 PC** 还是槽指令的 PC？MIPS 为此引入一个特殊 CSR (`EPC + BD bit`)——额外硬件

MIPS64 R6（2014）的决定：**把分支延迟槽废了**。RV32I 从一开始就没有——分支后面的指令该不跑就不跑。

**教训三：RISC-V 的结论**

这两个教训的交集是：**"PC 更新应该显式、局部、可预测"**。RV32I 给出的答案：

- PC **不是 GPR**（避开 R15 陷阱）
- 没有延迟槽（避开 MIPS 的语义扭曲）
- 分支/跳转**原子性地改 PC**（不存在"两条指令的联合分支"）

TEMU 的 `{pc, snpc, dnpc}` 三字段设计**正是这个 ISA 选择在软件层的对应**：

- `pc` 对应"这条指令的地址"——像 ARM 一样读当前 PC，不 +8，不 +4
- `snpc` 对应"纯顺序的下一条"——对应没有延迟槽时的 fall-through
- `dnpc` 对应"这条指令最终决定的下一条"——分支/跳转的唯一出口

**三字段既是 ISA 正确性的表达，也是代码可读性的工具**。如果我们只有 `cpu.pc`，就得在每条分支里手写"成立时跳、不成立时 +4"——等价于在软件里复刻 ARM R15 的所有坑。

> **工程原则**：**好的抽象层同时是历史教训的压缩包**。你用 `snpc/dnpc` 只要 2 分钟就学会；要理解它**为什么长这样**得读 30 年 ISA 史。这种"把教训藏在默认值里"的设计，让后人不用重蹈覆辙就能写对代码。

### 3.3 `cpu_exec` 主循环

P1 写过的循环，现在真正兑现：

```c
void cpu_exec(uint64_t n) {
    g.state = TEMU_RUNNING;
    for (uint64_t i = 0; i < n; i++) {
        exec_once();
        if (g.state != TEMU_RUNNING) break;
        if (wp_check()) { g.state = TEMU_STOP; break; }  /* P1 的 hook */
    }
    /* ... 打印 HIT END / HIT ABORT */
}
```

**P1 那行 `wp_check()`——从今天起第一次真的能触发**。指令真正修改了寄存器和内存，watchpoint 才有东西可以"被改"。

> **核心洞察**：`Decode { pc, snpc, dnpc, inst }` 是 TEMU 最重要的**通信约定**。`isa_exec_once` 读 `pc / snpc / inst`，写 `dnpc`（可选）和 `cpu.gpr[]`。这个"四字段契约"让我们能写 37 条独立指令而不互相干扰。

---

## 4. 指令编码家族：六种格式

37 条指令分为**六种编码格式**。格式不是"人为分类"——它由**指令需要哪些字段**决定。

### 4.1 六种格式一览

```
  31      25 24    20 19    15 14  12 11     7 6      0
┌──────────┬────────┬────────┬──────┬────────┬────────┐
R│ funct7   │  rs2   │  rs1   │funct3│   rd   │opcode  │   add/sub/sll...
├──────────┴────────┼────────┼──────┼────────┼────────┤
I│    imm[11:0]     │  rs1   │funct3│   rd   │opcode  │   addi/lw/jalr/csrrw...
├──────────┬────────┼────────┼──────┼────────┼────────┤
S│imm[11:5] │  rs2   │  rs1   │funct3│imm[4:0]│opcode  │   sw/sh/sb
├─┬────────┬────────┼────────┼──────┼────────┼─┬──────┤
B│i│imm     │  rs2   │  rs1   │funct3│imm     │i│opcode│   beq/bne...
 │12│[10:5] │        │        │      │[4:1]   │11│     │
├─┴────────┴────────┴────────┴──────┼────────┼────────┤
U│          imm[31:12]               │   rd   │opcode  │   lui/auipc
├─┬────────┬─┬──────────────────────┼────────┼────────┤
J│i│imm[10: │i│    imm[19:12]         │   rd   │opcode │   jal
 │20│1]     │11│                      │        │       │
└─┴────────┴─┴──────────────────────┴────────┴────────┘
```

### 4.2 为什么是这六种

关键观察：**rs1、rs2、rd 字段在所有用到它们的格式里都在固定位置**。`rs1` 永远是 bits [19:15]，`rs2` 永远是 [24:20]，`rd` 永远是 [11:7]。

**这不是巧合——这是硬件并行译码的条件**。译码时硬件可以**无条件**把 bits[19:15] 送去寄存器文件读 rs1，不需要先判断这条指令是 R/I/S/B 哪一种。

格式分类的真相是：**指令需要几个寄存器读端口 + 多少立即数位 = 决定哪个格式**。

| 格式 | 典型指令 | 寄存器字段 | 立即数宽度 | 特点 |
|------|---------|-----------|-----------|------|
| R | add / sub / sll | rs1, rs2, rd | 无 | 三地址寄存器运算 |
| I | addi / lw / jalr | rs1, rd | 12-bit signed | 一源 + 立即数 |
| S | sw / sh / sb | rs1, rs2 | 12-bit（拆成两段）| 两源，**无 rd**（store 不产生值）|
| B | beq / bne / ... | rs1, rs2 | 13-bit（LSB 固定 0）| 两源比较 + 分支偏移 |
| U | lui / auipc | rd | 20-bit（直接放高位）| 上位立即数 |
| J | jal | rd | 21-bit（LSB 固定 0）| 大跳转偏移 |

### 4.3 TEMU 里的格式声明

```c
// src/isa/riscv32/local-include/inst.h
typedef enum {
    TYPE_R, TYPE_I, TYPE_S, TYPE_B, TYPE_U, TYPE_J,
    TYPE_N,    /* no operands (ecall / ebreak / fence) */
} operand_type_t;

void decode_operand(Decode *s, operand_type_t type,
                    int *rd, word_t *src1, word_t *src2, word_t *imm);
```

一个 `decode_operand` 函数按 type 枚举分派——把寄存器和立即数解出来：

```c
// src/isa/riscv32/inst.c
void decode_operand(Decode *s, operand_type_t type,
                    int *rd, word_t *src1, word_t *src2, word_t *imm) {
    uint32_t i = s->inst;
    int rs1 = (int)BITS(i, 19, 15);
    int rs2 = (int)BITS(i, 24, 20);
    *rd   = (int)BITS(i, 11, 7);
    *src1 = 0; *src2 = 0; *imm = 0;

    switch (type) {
        case TYPE_R: *src1 = R(rs1); *src2 = R(rs2);                   break;
        case TYPE_I: *src1 = R(rs1); *imm  = immI(i);                  break;
        case TYPE_S: *src1 = R(rs1); *src2 = R(rs2); *imm = immS(i);   break;
        case TYPE_B: *src1 = R(rs1); *src2 = R(rs2); *imm = immB(i);   break;
        case TYPE_U: *imm  = immU(i);                                  break;
        case TYPE_J: *imm  = immJ(i);                                  break;
        case TYPE_N:                                                   break;
    }
}
```

**六种格式，七行代码**——因为 rs1/rs2/rd 字段位置固定，译码器的结构就是固定的。这正是 §4.4 理论视角要讲的。

### 4.4 理论视角：正交性与立即数的"乱序"

B-type 立即数的位映射长这样（从 `local-include/inst.h`）：

```c
#define immB(i) SEXT((BITS(i,31,31) << 12) |    /* 指令 bit 31 → imm bit 12 */
                     (BITS(i,7,7)   << 11) |    /* 指令 bit  7 → imm bit 11 */
                     (BITS(i,30,25) << 5)  |    /* 指令 bits[30:25] → imm[10:5] */
                     (BITS(i,11,8)  << 1),      /* 指令 bits[11:8]  → imm[4:1] */
                     13)
```

新手第一眼看会皱眉——**为什么不把 13 位立即数连续放一起？** 为什么拆成四段还交错摆？

答案是**正交性**。RISC-V 设计者优先保证：

- `rs1` 永远在 bits[19:15]
- `rs2` 永远在 bits[24:20]
- `funct3` 永远在 bits[14:12]
- `opcode` 永远在 bits[6:0]

这些字段**先"占座"**，立即数只能往**剩下的空隙**里塞。13 位立即数 = 8 + 5 个间断的位，没办法——就拆成四段。

**换来什么？** 硬件译码的**硅面积**：

- 由于 rs1/rs2/rd 位置固定，**无论指令是哪种格式，硬件都能把 bits[19:15] 无条件送去读 rs1**——不需要先判断格式
- 立即数的每一位都来自指令字的**固定位置**——一根导线直连，不需要 mux 选择
- "判断格式"只影响"要不要用 rs2 / 立即数 / rd"，不影响"从哪读"——这让译码可以**完全并行**

对比：x86 有变长编码（1-15 字节）+ prefix + ModR/M + SIB + 变长立即数——这是为什么 x86 译码器要数千门逻辑，而 RV32I 译码器几百门就够了。

> **核心洞察**：正交性不是美学选择，它是**硅面积**的直接体现。立即数看起来"乱"，是因为它**让位**给了寄存器字段的固定位置。一根走线省的面积，乘以每个芯片上千万个译码器副本，就是晶圆的实际成本。

---

## 5. 立即数解码：SEXT 宏

### 5.1 `SEXT` 符号扩展的"一行魔法"

```c
// src/isa/riscv32/local-include/inst.h
#define SEXT(x, n) \
    ((word_t)((int32_t)((uint32_t)(x) << (32 - (n))) >> (32 - (n))))
```

三步变换：

1. 把 `n` 位的值左移 `32 - n` → 原来的最高位（符号位）现在在 bit 31
2. 强转 `int32_t` 后**算术右移** `32 - n` → 右移过程中 bit 31 被**重复复制**到高位
3. 转回 `word_t` 返回

一条表达式，无分支，无条件判断。是所有 RISC 模拟器的"第二常用代码"（第一是 `guest_to_host`）。

**Caveat**：C 标准对"有符号数右移"是 implementation-defined。但在 **clang / gcc / MSVC 的所有目标平台**上，它都是算术右移。我们依赖这个约定。

**C23 的未来**：C23 终于把"有符号右移 = 算术右移"写进标准（ISO/IEC 9899:2024 § 6.5.7）。在那之前（C11/C17），技术上我们在"标准缝隙"里走——但所有主流编译器过去 30 年都一致，工程上安全。一个对比：如果你真的担心可移植性，有完全 UB-free 的写法：

```c
#define SEXT_PURE(x, n) \
    (((word_t)(x) & (1u << ((n)-1))) \
        ? ((word_t)(x) | ((word_t)-1 << (n))) \
        : ((word_t)(x) & ((1u << (n)) - 1)))
```

三元运算符分"正负"两支，无任何 UB。缺点：编译器生成的代码**不如左移-右移的一句好**——现代 x86 / ARM / RISC-V 都有单周期算术移位指令。所以工程选择是 "依赖 implementation-defined 但拿 1 条指令" 而不是 "100% 标准但拿 4-5 条指令"。

> **工程原则**：C 的 UB 和 implementation-defined behavior 不是同一个概念。**UB 不可依赖**（编译器优化可能假设不发生），**implementation-defined 可依赖**（编译器必须稳定、且文档化）。TEMU 的 SEXT 踩在后者——允许。

### 5.2 naive vs refined：立即数提取

初学者的写法：

```c
/* v1 naive — 手动 bit surgery，不要这样写 */
static void execute_addi(uint32_t inst, ...) {
    int rs1 = (inst >> 15) & 0x1f;
    int rd  = (inst >>  7) & 0x1f;
    int imm = (inst >> 20);                 /* 12-bit */
    if (imm & 0x800) imm |= 0xFFFFF000;     /* 手动符号扩展 */
    /* ... */
}
```

问题：

- 每条 I-type 指令都要重复这三行
- B-type / J-type 的拆分更复杂，重复 6 种格式 × 37 条 ≈ 几百行 bit surgery
- bit surgery 写错编译器不会报错——SEXT 符号扩展位数差 1 就是静默 bug

**v2 正式版**：为每种格式写**一个宏**，全仓库用 37 次：

```c
// src/isa/riscv32/local-include/inst.h
#define BITS(x, hi, lo) \
    (((uint32_t)(x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

#define immI(i) SEXT(BITS(i, 31, 20), 12)

#define immS(i) SEXT((BITS(i, 31, 25) << 5) | BITS(i, 11, 7), 12)

#define immB(i) SEXT((BITS(i, 31, 31) << 12) | (BITS(i, 7,  7) << 11) |
                     (BITS(i, 30, 25) << 5)  | (BITS(i, 11, 8) << 1),  13)

#define immU(i) ((word_t)(BITS(i, 31, 12) << 12))

#define immJ(i) SEXT((BITS(i, 31, 31) << 20) | (BITS(i, 19, 12) << 12) |
                     (BITS(i, 20, 20) << 11) | (BITS(i, 30, 21) << 1),  21)
```

注意两个细节：

- **B-type 和 J-type 缺 bit 0**——因为分支/跳转目标必须 2 字节对齐，LSB 永远是 0，spec 干脆**不把它编进指令**。`immB` 里最低的 shift 是 `<< 1`（产生 bit 1）
- **U-type 不符号扩展**——`lui a0, 0x12345` 的意图就是"把 0x12345 放到 a0 的高 20 位"，不涉及正负号

---

## 6. 模式匹配派发：INSTPAT

### 6.1 naive vs refined（大头）

**v1 naive：嵌套 switch**

```c
/* v1 朴素版 —— 不要这样写 */
switch (BITS(inst, 6, 0)) {                 /* opcode */
    case 0x13: {                             /* OP-IMM */
        switch (BITS(inst, 14, 12)) {       /* funct3 */
            case 0x0: /* addi */    break;
            case 0x1:                        /* SLLI —— 还要看 funct7 */
                switch (BITS(inst, 31, 25)) {
                    case 0x00: /* slli */ break;
                    default: invalid();
                }
                break;
            /* ... 6 more funct3 cases ... */
        }
        break;
    }
    case 0x33: {                             /* OP */
        /* 同样三层 switch */
    }
    /* ... 每个 opcode 一段这样的代码 ... */
}
```

问题：

- **三层嵌套** × **多个 opcode 家族**——几百行难以阅读
- 加一条指令要改 1-3 处 switch
- **看不到"这条指令的完整规则是什么"**——规则散在三层里

**v2 refined：INSTPAT 模式字符串**

每条指令**一行**，模式字符串直接就是译码规则：

```c
INSTPAT("??????? ????? ????? 000 ????? 0110011", "add",  R,
        R(rd) = src1 + src2);
INSTPAT("0100000 ????? ????? 000 ????? 0110011", "sub",  R,
        R(rd) = src1 - src2);
```

模式字符串按 MSB → LSB 读，`0` / `1` / `?`：

- `funct7(7) rs2(5) rs1(5) funct3(3) rd(5) opcode(7)` = 32 bits
- `0`/`1` 是必须匹配；`?` 是 don't-care
- 每条指令都是 "pattern + 名字 + 格式 + 语义表达式" 的四元组

### 6.2 `pattern_match` 函数

```c
// src/isa/riscv32/inst.c
bool pattern_match(uint32_t inst, const char *pat) {
    uint32_t key = 0, mask = 0;
    int bit = 31;
    for (const char *p = pat; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '\t') continue;        /* 空白忽略 */
        if      (c == '?') { /* don't care */ }
        else if (c == '0') { mask |= (1u << bit); }
        else if (c == '1') { mask |= (1u << bit); key  |= (1u << bit); }
        else panic("bad character '%c' in pattern '%s'", c, pat);
        bit--;
    }
    return (inst & mask) == key;
}
```

**本质是一句**：`(inst & mask) == key`。

- `mask` = "我关心哪些位"
- `key` = "这些位应该是什么值"
- `?` 对应 mask 里的 0——这位**不关心**

### 6.3 `INSTPAT` 宏

```c
#define INSTPAT(pat, mnem, ty, body)                                 \
    if (!matched && pattern_match(inst, pat)) {                      \
        decode_operand(s, TYPE_##ty, &rd, &src1, &src2, &imm);       \
        g_last_disasm.name = (mnem);                                 \
        g_last_disasm.type = TYPE_##ty;                              \
        g_last_disasm.inst = inst;                                   \
        g_last_disasm.pc   = s->pc;                                  \
        g_last_disasm.rd   = rd;                                     \
        g_last_disasm.rs1  = (int)BITS(inst, 19, 15);                \
        g_last_disasm.rs2  = (int)BITS(inst, 24, 20);                \
        g_last_disasm.imm  = imm;                                    \
        body;                                                        \
        matched = true;                                              \
    }
```

`body` 是一小段 C 代码——在 `INSTPAT` 展开的地方，`rd` / `src1` / `src2` / `imm` 都已经被 `decode_operand` 设置好了，`body` 可以直接用。

**first match wins**：从上往下扫描，第一个匹配的就执行。**顺序是语义**——SUB（funct7=0100000）必须在 ADD 之前，不然会被后者"抢先"。

### 6.4 理论视角：模式匹配 ≡ PLA ≡ 真值表

我们写的 `pattern_match` 和硬件 CPU 的 **PLA**（Programmable Logic Array，可编程逻辑阵列）是同一个抽象的两种实现。

**PLA 是什么**：一种标准数字逻辑模块，由一层 AND 门 + 一层 OR 门组成，"可编程"的部分是选择哪些连接导通。任何真值表都能编成 PLA。

**我们的 `(inst & mask) == key` 本质**：一个 **AND-term** ——"这些位必须等于这些值"。37 条指令 = 37 个 AND-term。

```
软件（顺序扫描）                 硬件（PLA，并行）

 for i = 0..36:                     inst[31:0]
    if (inst & mask[i]) == key[i]:     │
        match[i] = 1                  ┌┴───────┐
        break                         │ PLA    │
                                      │ 37 行  │
                                      └┬───────┘
                                        │
                                      match[0..36]   一个周期完成 37 次匹配
```

**算法等价**：

- 软件 O(N)：最多扫描 N 条模式才找到
- 硬件 O(1)：所有模式**并行**匹配，一个门延迟

**历史**：早期 Intel 芯片（8086、80286）用 PLA 做指令译码。现代 x86 因为变长编码用 PLA 不合算，改成 **快速路径 PLA + 复杂指令微码 ROM** 混合。RISC-V 由于编码规整，纯 PLA 就能覆盖全部译码——**我们的代码规模和它的硬件门数对比是 ~100 倍到 ~10000 倍的差距**，但规范描述是相同的 37 条规则。

> **核心洞察**：**你的 INSTPAT 表就是一张纸上的 PLA**。每条模式 = 一根 AND 线；`first match wins` = 硬件的**优先级编码器**。软件顺序扫 + 硬件并行匹——是一回事的两种实现。

---

## 7. 实现指令集

### 7.1 `isa_exec_once` 骨架

```c
// src/isa/riscv32/inst.c
int isa_exec_once(Decode *s) {
    uint32_t inst = s->inst;
    int    rd   = 0;
    word_t src1 = 0, src2 = 0, imm = 0;
    bool   matched = false;

    #define INSTPAT(pat, mnem, ty, body) /* ... as above ... */

    /* --- upper-immediate (U-type) ---------------------------------- */
    INSTPAT("??????? ????? ????? ??? ????? 0110111", "lui",   U,
            R(rd) = imm);
    INSTPAT("??????? ????? ????? ??? ????? 0010111", "auipc", U,
            R(rd) = s->pc + imm);

    /* 其余 35 条接着 INSTPAT 下去 ... */

    /* --- catch-all: anything not matched above is illegal ---------- */
    INSTPAT("??????? ????? ????? ??? ????? ???????", "invalid", N,
            invalid_inst(s));

    #undef INSTPAT
    return 0;
}
```

下面分组展开 37 条。

### 7.2 算术逻辑：I/R-type（19 条）

**I-type 算术**（6 条，带 12-bit 立即数）：

```c
INSTPAT("??????? ????? ????? 000 ????? 0010011", "addi",  I,
        R(rd) = src1 + imm);
INSTPAT("??????? ????? ????? 010 ????? 0010011", "slti",  I,
        R(rd) = (sword_t)src1 <  (sword_t)imm ? 1 : 0);
INSTPAT("??????? ????? ????? 011 ????? 0010011", "sltiu", I,
        R(rd) = src1 < imm ? 1 : 0);
INSTPAT("??????? ????? ????? 100 ????? 0010011", "xori",  I,
        R(rd) = src1 ^ imm);
INSTPAT("??????? ????? ????? 110 ????? 0010011", "ori",   I,
        R(rd) = src1 | imm);
INSTPAT("??????? ????? ????? 111 ????? 0010011", "andi",  I,
        R(rd) = src1 & imm);
```

注意 `slti` / `sltiu` 的区别——前者**有符号**比较（需要 `(sword_t)` 强转），后者**无符号**。立即数在两者里**都是符号扩展**后再参与比较（spec 规定）——所以 `sltiu a0, zero, -1` 是 `0 < 0xffffffff` 为真。

**I-type 移位**（3 条，`shamt = imm[4:0]`）：

```c
INSTPAT("0000000 ????? ????? 001 ????? 0010011", "slli", I,
        R(rd) = src1 << BITS(imm, 4, 0));
INSTPAT("0000000 ????? ????? 101 ????? 0010011", "srli", I,
        R(rd) = src1 >> BITS(imm, 4, 0));
INSTPAT("0100000 ????? ????? 101 ????? 0010011", "srai", I,
        R(rd) = (word_t)((sword_t)src1 >> BITS(imm, 4, 0)));
```

`srai`（算术右移）用 funct7 = `0100000` 和 `srli` 区分。spec 规定 RV32I 的移位量只取低 5 位。

**R-type 算术**（10 条）：

```c
INSTPAT("0000000 ????? ????? 000 ????? 0110011", "add",  R,
        R(rd) = src1 + src2);
INSTPAT("0100000 ????? ????? 000 ????? 0110011", "sub",  R,
        R(rd) = src1 - src2);
INSTPAT("0000000 ????? ????? 001 ????? 0110011", "sll",  R,
        R(rd) = src1 << (src2 & 31));
INSTPAT("0000000 ????? ????? 010 ????? 0110011", "slt",  R,
        R(rd) = (sword_t)src1 <  (sword_t)src2 ? 1 : 0);
INSTPAT("0000000 ????? ????? 011 ????? 0110011", "sltu", R,
        R(rd) = src1 < src2 ? 1 : 0);
INSTPAT("0000000 ????? ????? 100 ????? 0110011", "xor",  R,
        R(rd) = src1 ^ src2);
INSTPAT("0000000 ????? ????? 101 ????? 0110011", "srl",  R,
        R(rd) = src1 >> (src2 & 31));
INSTPAT("0100000 ????? ????? 101 ????? 0110011", "sra",  R,
        R(rd) = (word_t)((sword_t)src1 >> (src2 & 31)));
INSTPAT("0000000 ????? ????? 110 ????? 0110011", "or",   R,
        R(rd) = src1 | src2);
INSTPAT("0000000 ????? ????? 111 ????? 0110011", "and",  R,
        R(rd) = src1 & src2);
```

`sll` / `srl` / `sra` 的移位量用 `src2 & 31`——硬件只看低 5 位。spec 原话：*"The operand to be shifted is in rs1, and the shift amount is the low 5 bits of rs2"*。

### 7.2.1 理论视角：为什么 SLT 和 SLTU 都要有

**问题动机**

一个 32-bit 寄存器放着 `0x80000000`。它是 `-2147483648` 还是 `2147483648`？

硬件**不知道**——寄存器只是 32 根线，电平本身没有"有符号"属性。解释权在指令手里：`slt` 说"按 signed 读"，`sltu` 说"按 unsigned 读"。于是同一对输入能给出不同答案：

| 输入 src1 / src2 | `slt` | `sltu` |
|--|--|--|
| `0x80000000` / `0x00000001` | 1（`-2³¹ < 1`）| 0（`2³¹ > 1`）|
| `0xFFFFFFFF` / `0x00000001` | 1（`-1 < 1`）| 0（`2³²-1 > 1`）|
| `0x00000005` / `0x00000003` | 0 | 0 |

⚠️ **常见误解**：以为"signed 和 unsigned 只是加法 / 乘法的溢出行为不一样，比较操作应该共用一条指令"。错。**加减乘**这种"按 mod 2³² 运算"的操作，signed 和 unsigned 的结果**bit-pattern 完全相同**——两者只是对最终那块位的**解读**不同（overflow flag 才体现差异）。但**比较**直接产出 0 或 1，不同解读给**不同的 bit 输出**，没法复用同一条硬件路径。

**Naive 方案**：只留 `sltu`，signed 比较由编译器合成

硬件能不能省一条？能，但代价很难看。要做 signed 比较，先把两个操作数都异或 `0x80000000`（把 signed 区间平移到 unsigned 区间），再做 unsigned 比较：

```
slt rd, a, b
    ↓ 翻译
lui  t0, 0x80000       # t0 = 0x80000000          1 条
xor  t1, a, t0         # t1 = a ^ 0x80000000      1 条
xor  t2, b, t0                                    1 条
sltu rd, t1, t2                                   1 条
# 总共 4 条，还污染了两个临时寄存器
```

一条 signed 比较从 1 条指令变 4 条，还要占用 `t0`/`t1`/`t2`——代码密度崩盘，寄存器分配器痛苦。RISC-V 的哲学是"让**常见操作是 1 条指令**"，这条省不得。

**关键洞察**：Z/2³² 上的"小于"不唯一

把 32-bit 寄存器看作商群 Z/2³²Z，**这个群本身没有序**——它是循环的。"小于"是额外加上去的全序关系，取决于你"在哪个位置切开这个环"：

```
unsigned 切在 0 和 2³²-1 之间：
  0  1  2  ...  2³¹-1  2³¹  ...  2³²-1 | 0
  └──────── 从小到大 ────────────────┘

signed 切在 2³¹-1 和 2³¹ 之间：
  2³¹  2³¹+1  ...  2³²-1 | 0  1  ...  2³¹-1
  (-2³¹)(-2³¹+1)        (-1)(0)(1)     (+2³¹-1)
  └──────── 从小到大 ────────────────┘
```

两种切法都合法，**只是两条并列的总序**。硬件必须给程序员自选权——所以 `slt` 和 `sltu` 是**一对**，不是"主-辅"关系。分支指令 `blt/bltu`、`bge/bgeu` 同样成对，原因一样。

**Connections：RISC-V 无 flags vs x86 有 flags**

x86 的做法完全不同：

```asm
cmp  eax, ebx       ; 一条 CMP，设置 ZF / SF / CF / OF 四个 flag
jl   label          ; signed less：看 SF != OF
jb   label          ; unsigned below：看 CF == 1
```

**一条比较 + 两种分支**，signed / unsigned 的选择推迟到 branch。代价是：

- 架构状态里多了 4 个 flag 位（软件上下文切换必须保存 / 恢复）
- 大部分指令都会"顺便"写 flag（加减乘除全都动），导致编译器做指令调度时多出一堆隐式依赖

RISC-V 反过来：**没有全局 flag，比较结果写进通用寄存器**。代价是需要 slt/sltu 两条指令，收益是：

1. 任何指令之间都没有隐式依赖（流水线调度干净）
2. 上下文切换不用存 flag
3. 超标量乱序执行时，没有"谁最后写了 flag"这种 reorder 障碍

**取舍标签**：theoretical ideal。RISC-V 宁可多一条 funct3 编码位、多一条指令名，也要把架构状态压到最小。现代高主频 / 乱序 CPU 的调度成本远高于"多一条指令"的代码密度损失——2015 年以后的 benchmark 已经反复验证这条。

**Limitations**

- `sltiu rd, rs, 1` 是个惯用法：等价于 `rd = (rs == 0)`。因为 unsigned 世界里只有 0 "小于 1"。这是 SLTU **存在**而不是 SLT 之外**附加**的小副产品
- 立即数在 `sltiu` 里**依然是符号扩展**的（spec 明确），所以 `sltiu rd, rs, -1` 实际是 `rs <u 0xFFFFFFFF` ≡ `rs != 0xFFFFFFFF`。这个点常被初学者当 bug 报
- 没有"signed greater than" 指令——用 `slt rd, rs2, rs1` 交换操作数就行。spec 故意不给，避免指令集膨胀

### 7.3 分支：B-type（6 条）

```c
INSTPAT("??????? ????? ????? 000 ????? 1100011", "beq",  B,
        if (src1 == src2) s->dnpc = s->pc + imm);
INSTPAT("??????? ????? ????? 001 ????? 1100011", "bne",  B,
        if (src1 != src2) s->dnpc = s->pc + imm);
INSTPAT("??????? ????? ????? 100 ????? 1100011", "blt",  B,
        if ((sword_t)src1 <  (sword_t)src2) s->dnpc = s->pc + imm);
INSTPAT("??????? ????? ????? 101 ????? 1100011", "bge",  B,
        if ((sword_t)src1 >= (sword_t)src2) s->dnpc = s->pc + imm);
INSTPAT("??????? ????? ????? 110 ????? 1100011", "bltu", B,
        if (src1 <  src2) s->dnpc = s->pc + imm);
INSTPAT("??????? ????? ????? 111 ????? 1100011", "bgeu", B,
        if (src1 >= src2) s->dnpc = s->pc + imm);
```

**`s->dnpc` 的"静默默认"在这里闪光**：`exec_once` 开头已经把 `s->dnpc = s->snpc`。分支指令**只在条件成立时覆盖**，条件不成立什么都不用做——**fall-through 是免费的**。这就是 §3.2 铺垫的 refined 设计的回报。

`blt/bge` 用 `(sword_t)` 是**有符号**比较；`bltu/bgeu` 不强转是**无符号**。spec 明确区分。

### 7.4 跳转：JAL 和 JALR

```c
INSTPAT("??????? ????? ????? ??? ????? 1101111", "jal",  J, {
    word_t link = s->snpc;          /* ① 先存 link（= pc + 4）*/
    s->dnpc = s->pc + imm;           /* ② 再改 dnpc */
    R(rd) = link;                    /* ③ 写 link 寄存器 */
});
INSTPAT("??????? ????? ????? 000 ????? 1100111", "jalr", I, {
    word_t link = s->snpc;
    s->dnpc = (src1 + imm) & ~(word_t)1;   /* 强制低位清零 */
    R(rd) = link;
});
```

两处**subtle but critical**：

1. **`link = s->snpc`，不是 `s->pc`**：链接寄存器（通常 `ra = x1`）应该指向**跳转指令的下一条**，不是跳转本身。写错的话函数 `ret` 返回到 `jal` 上，无限递归。
2. **`& ~(word_t)1`**：`jalr` 目标**强制 2 字节对齐**。spec 要求的——为了兼容未来压缩指令（C extension 允许 2 字节指令）。即使纯 RV32I 我们也按 spec 写，签合同要守字条。

先存 `link` 再改 `dnpc` 是**显式的写入顺序**——即使今天无所谓，这种"捕获所有输入后再改输出"的模式是状态机代码的好习惯。

**调用-返回示意**：

```
0x80000000  jal   ra, fib        ; ra = 0x80000004, pc = fib
0x80000004  addi  a0, a0, 1      ; 返回点
...
fib:        ...
            jalr  x0, ra, 0       ; pc = ra = 0x80000004, rd=x0 所以 link 丢弃
```

`jalr x0, ra, 0` 就是"return"——跳到 ra，不需要链接（rd=x0 丢弃）。

### 7.5 Load / Store（8 条）

**Load**（5 条，从内存读到寄存器）：

```c
INSTPAT("??????? ????? ????? 000 ????? 0000011", "lb",  I,
        R(rd) = SEXT(paddr_read(src1 + imm, 1),  8));
INSTPAT("??????? ????? ????? 001 ????? 0000011", "lh",  I,
        R(rd) = SEXT(paddr_read(src1 + imm, 2), 16));
INSTPAT("??????? ????? ????? 010 ????? 0000011", "lw",  I,
        R(rd) = paddr_read(src1 + imm, 4));
INSTPAT("??????? ????? ????? 100 ????? 0000011", "lbu", I,
        R(rd) = paddr_read(src1 + imm, 1));
INSTPAT("??????? ????? ????? 101 ????? 0000011", "lhu", I,
        R(rd) = paddr_read(src1 + imm, 2));
```

三处关键：

- **`lb` / `lh` 符号扩展**——读一字节 `0xFF` 应得 `0xFFFFFFFF`（-1），而不是 `0x000000FF`
- **`lbu` / `lhu` 零扩展**——P2 里 `paddr_read` 内部 `word_t ret = 0` **自动零扩展**高位。**P2 埋的伏笔在这里兑现**：无符号 load 不用任何额外代码
- **`lw` 不需要扩展**——已经是 32 位

**Store**（3 条，从寄存器写进内存）：

```c
INSTPAT("??????? ????? ????? 000 ????? 0100011", "sb", S,
        paddr_write(src1 + imm, 1, src2));
INSTPAT("??????? ????? ????? 001 ????? 0100011", "sh", S,
        paddr_write(src1 + imm, 2, src2));
INSTPAT("??????? ????? ????? 010 ????? 0100011", "sw", S,
        paddr_write(src1 + imm, 4, src2));
```

S-type 编码：**rs2 是数据源**（要写的值），rs1 是基址。立即数拆成两段 `[31:25]` 和 `[11:7]`——`immS` 宏负责拼回来。

**没有 `lwu` / `sw` 不需要变种**——lwu 在 RV64 里才有（32→64 的零扩展），RV32 的 `lw` 已经是全宽度。

### 7.6 FENCE / EBREAK / 非法指令

```c
/* FENCE 和 FENCE.I：内存屏障，我们的模拟器是顺序一致的，NOP */
INSTPAT("??????? ????? ????? 000 ????? 0001111", "fence",   I, (void)0);
INSTPAT("??????? ????? ????? 001 ????? 0001111", "fence.i", I, (void)0);

/* EBREAK：简化版——halt 模式下直接结束,halt_ret = a0 */
INSTPAT("0000000 00001 00000 000 00000 1110011", "ebreak", N,
        temu_set_end(s->pc, R(10)));

/* 兜底：任何上面没匹配的指令 */
INSTPAT("??????? ????? ????? ??? ????? ???????", "invalid", N,
        invalid_inst(s));
```

**FENCE**：真硬件上 FENCE 保证内存操作的可见性次序（多核场景）；FENCE.I 保证 "写指令内存" 和 "取指" 的次序（自修改代码）。**我们的模拟器单线程 + 每次取指都从 pmem 新读**，两件事天然满足——NOP 合法。

**EBREAK**：用于测试退出。调用约定：把返回值放 `a0 = x10`，执行 `ebreak`。`temu_set_end` 打印 `HIT END halt_ret=0x...` 并让 `cpu_exec` 循环停下。**完整的 EBREAK 语义**（作为 BREAKPOINT 异常陷入）要等 P6a。

**`invalid_inst`**：打印红字错误，设 `TEMU_ABORT` 状态——**不 panic**，这样 P1 的调试器还能被用来检查寄存器。

---

## 8. x0 硬连线：分层的兑现

P2 承诺过：**"x0 硬连线为 0 是 ISA 语义，不是 C 数据结构的职责——P3 的指令执行路径处理"**。

### 8.1 两种实现的对比

**v1 naive：每个写入点都 guard**

```c
/* 不要这样 */
if (rd != 0) R(rd) = src1 + imm;    /* ADDI */
if (rd != 0) R(rd) = src1 + src2;   /* ADD */
/* ... 37 条指令里 30+ 条有 rd 写入 ... */
```

**v2 refined：循环末尾一次清零**

```c
// src/cpu/cpu_exec.c
isa_exec_once(&s);              /* 指令随便写 R(0) */
if (g.state == TEMU_RUNNING) cpu.pc = s.dnpc;
cpu.gpr[0] = 0;                 /* 一行覆盖所有 "写 x0" 的企图 */
```

### 8.2 为什么 v2 赢

- **一行 vs 37 处**——维护成本相差 30 倍
- **INSTPAT body 保持 clean**——指令语义不被"特殊 case"污染
- **没有"写 x0 的中间可观测状态"**：`isa_exec_once` 返回前后**没有别的代码跑**，gpr[0] = 0 立刻生效，外部不可见

> **工程原则**：**不变量应该在循环边界实现，不在每个调用点**。这条原则和 P2 的 "显式 `cpu_init` 替代隐式零初始化" 是同一个品味——控制流的"接缝处" 是放置不变量的正确位置。

### 8.3 理论视角：零寄存器不是"优化"，是**编码省料**

上面 §8.2 只讲了"模拟器端怎么实现 x0 最干净"。但还有一个更上游的问题：**ISA 为什么要**有一个硬连线为 0 的寄存器？少一个可用寄存器明明是**损失**——把它做成真正能读能写不是更好？

答案要从**指令编码的稀缺性**说起。

**问题动机：编码位是稀缺资源**

RV32I 指令 32-bit 固定宽度。R-type 格式必须塞下：`opcode(7) + rd(5) + rs1(5) + rs2(5) + funct3(3) + funct7(7)` = 32 位，**一位不剩**。ISA 设计者要不断问：

- 要不要给"无条件跳转"一个单独 opcode？
- 要不要给"寄存器移动 `mv rd, rs`"一个单独 opcode？
- 要不要给"比较与零"一组单独的分支？

每加一个特殊 opcode 就消耗一个**主 opcode 槽位**（7 位 → 128 个槽位，RV32I 已经用掉约 32 个）。主 opcode 槽位不够用 → 压力转移到 funct3/funct7 扩展字段 → 扩展字段耗尽就挡住未来指令集扩展（M/A/F/D/C/V …）。

**Naive 方案**：给每个伪指令单独编码

```
MOV   rd, rs       → 一个专用 opcode
NOP                → 一个专用 opcode
J     target       → 无条件跳转专用 opcode
RET                → 返回专用 opcode
BEQZ  rs, target   → 和零比较专用 opcode
NEG   rd, rs       → 取反专用 opcode
...
```

粗略算一下：典型汇编代码里 **30% 是 MV**，**15% 是 NOP / 对齐填充**，**10% 是无条件跳转或 BEQZ/BNEZ**。给这些指令都做专用 opcode，主 opcode 表至少要多出 6–8 个槽位。代价：

- 解码器要多处理 6–8 条路径，流水线前端变宽
- 编译器后端多维护一套"伪指令能否合并"的规则
- 未来的扩展指令集没地方放

**关键洞察**：把一个寄存器"烧掉"换几十条伪指令

让 `x0` 恒为 0、写入丢弃——**一个寄存器名换来一整族伪指令免编码**：

| 伪指令（汇编写法）| 真实机器码 | 省掉的专用 opcode |
|--|--|--|
| `nop` | `addi x0, x0, 0` | NOP opcode |
| `mv rd, rs` | `addi rd, rs, 0` | MOV opcode |
| `li rd, imm` （小立即数）| `addi rd, x0, imm` | LOAD-IMMEDIATE opcode |
| `not rd, rs` | `xori rd, rs, -1` | （复用 XORI）|
| `neg rd, rs` | `sub rd, x0, rs` | NEG opcode |
| `seqz rd, rs` | `sltiu rd, rs, 1` | SET-EQ-ZERO opcode |
| `snez rd, rs` | `sltu rd, x0, rs` | SET-NE-ZERO opcode |
| `beqz rs, lbl` | `beq rs, x0, lbl` | BEQZ opcode |
| `bgez rs, lbl` | `bge rs, x0, lbl` | BGEZ opcode |
| `j target` | `jal x0, target` | J opcode（丢弃 link）|
| `jr rs` | `jalr x0, rs, 0` | JR opcode |
| `ret` | `jalr x0, x1, 0` | RET opcode |

**13 条伪指令被 0 条专用 opcode 吸收**。这是一笔极其划算的生意——**sacrifice 1 register name 换 ~13 个 opcode slots**。

**信息论视角**：R-type 的 `rd` 字段是 5 位 = 32 个值。`rd = 0` 这个值原本承载"写到 x0 寄存器"的语义，**边际使用频率极低**（程序员几乎不会真的想要这个目标寄存器）。把这个"几乎浪费"的编码点重新定义为"丢弃结果"，**把字段空间榨干**——教科书级的有损编码复用。

**⚠️ 常见误解**

> "x0 只是运行时优化，用来快速得到 0。"

❌ **反了**。想"快速得到 0" 的话，ADD reset-to-zero 电路根本不需要一个专用寄存器——ARM 的 `MOV Rd, #0` 就是一条两字节指令，CPU 自己内部置零。x0 的**真正作用是指令编码**，运行时性能只是副产品。

> "AArch64 的 XZR 就是抄 RISC-V 的。"

❌ 反了。**MIPS-I（1985）**首先系统化了"hardwired-zero 寄存器 + 伪指令族"的思路（`$zero` = `$0`）。RISC-V 继承 MIPS 传统。ARM 早期（A32/T32）**没有**零寄存器，所有 NOP/MOV 都是真实指令；直到 AArch64（2011）才加入 XZR/WZR——正是看到了 MIPS/RISC-V 路线的编码收益。

**Connections：x0 和 x1 是一对编码捷径**

| 寄存器 | 语义 | 带来的伪指令 |
|--|--|--|
| `x0` | hardwired zero，写入丢弃 | nop / mv / j / ret / seqz / bnez / neg … |
| `x1` | ABI 约定的 return address（ra）| `jal x1, f` = call；`jalr x0, x1, 0` = ret |

两者**组合**才让 `call` / `ret` 免专用 opcode：`jal` 和 `jalr` 各自只是"跳转+链接到任意寄存器"的原子操作，把 link 寄存器选成 `x1` 或 `x0`，就免费得到函数调用/返回语义。**这不是巧合——是有意的编码层解耦**：CPU 硬件不区分"call / ret / jump"，编码靠"哪个寄存器当 link" 区分，reset 了整个跳转族。

**取舍标签**：theoretical ideal。x0 的代价是**31 个可用寄存器而不是 32 个**——在寄存器分配压力极高的代码里（比如 register-heavy 的 numerical kernel），少一个寄存器会多一次 spill。但 RISC 哲学认为：**编码空间的节省**让未来扩展成为可能（M/F/D/V …），远比"多一个 GPR" 重要。2020 年 RV32I 能无痛扩 vector / 压缩 / 向量扩展，和这条设计决定直接相关。

**Limitations**

- `x0` 不能做函数参数传递的容器——ABI 里 x10–x17 是 arg，不包括 x0
- 写入 x0 **不算 NOP 副作用**：`add x0, x1, x2` 的 add 本身被 CPU 跑完了（加法器能量、流水线槽位）——只是结果被丢。现代实现（OoO CPU）会在 rename 阶段把 dest=x0 检测出来、不分配物理寄存器；但在 in-order 小核上就真的是白烧电。
- ABI 保留 `x0` 永远是 0 是**软硬契约**：软件约定永远不把任何有意义值放 x0（因为放了也被丢）。这条没法从硬件语义推出——硬件只保证"读 x0 返回 0"，约定链是"没人写有意义的东西 → 所以丢不丢不重要 → 所以我们可以假设恒为 0"。

---

## 9. Itrace：第一个真实的调试工具

P2 讲过"itrace 还没数据"。P3 的 `exec_once` 跑起来，ring buffer 就有东西可写了：

```c
// src/cpu/cpu_exec.c
#define ITRACE_SIZE     16
#define ITRACE_DISASM   48

static struct {
    vaddr_t  pc;
    uint32_t inst;
    char     disasm[ITRACE_DISASM];
} itrace_ring[ITRACE_SIZE];
static uint64_t itrace_head = 0;

static void itrace_record(vaddr_t pc, uint32_t inst) {
    size_t idx = (size_t)(itrace_head % ITRACE_SIZE);
    itrace_ring[idx].pc   = pc;
    itrace_ring[idx].inst = inst;
    disasm(itrace_ring[idx].disasm, ITRACE_DISASM, &g_last_disasm);
    itrace_head++;
}
```

Ring buffer 模式：`head % SIZE` 做下标，旧条目被新的默默覆盖。`head` 单调递增——"现有多少条" = `min(head, SIZE)`。

**触发条件**：`cpu_exec` 异常结束（invalid inst、watchpoint、ebreak abort）时 dump 最近 16 条：

```
--- itrace (last 16 instructions) ---
  0x80000000:  00500513  addi    a0, zero, 5
  0x80000004:  00a00613  addi    a2, zero, 10
  0x80000008:  00c50733  add     a4, a0, a2
  ...
```

`g_last_disasm` 是一个全局——`INSTPAT` 宏里每次匹配都会覆盖写它，记录最近一条指令的译码信息。itrace 把 `g_last_disasm` 喂给 `disasm()` 格式化输出。

**Itrace 大小是 tunable 的**——Medium 练习会让你通过命令行改它。

---

## 10. 跑真程序：fib 和 sum

到了兑现的时刻。我们手写一个 fib(10)，**不调用 gcc**，就用 `isa-encoder.h` 里的编码宏：

```c
// tests/isa/isa-encoder.h 片段
#define ADDI(rd, rs1, imm)   itype(imm, rs1, 0x0, rd, 0x13)
#define ADD(rd, rs1, rs2)    rtype(0x00, rs2, rs1, 0x0, rd, 0x33)
#define BEQ(rs1, rs2, imm)   btype(imm, rs2, rs1, 0x0, 0x63)
#define JAL(rd, imm)         jtype(imm, rd, 0x6f)
/* ... */
```

**iterative fib(10)**：

```c
RUN("iterative fib(10) = 55", 55,
    ADDI(A0, ZERO, 10),              /* n = 10             */
    ADDI(T0, ZERO, 0),               /* a = 0              */
    ADDI(T1, ZERO, 1),               /* b = 1              */
    BEQ(A0, ZERO, 24),               /* loop: if n==0 done */
    ADD(T2, T0, T1),                 /*   t = a + b        */
    ADD(T0, ZERO, T1),               /*   a = b            */
    ADD(T1, ZERO, T2),               /*   b = t            */
    ADDI(A0, A0, -1),                /*   n--              */
    JAL(ZERO, -20),                  /*   goto loop        */
    ADD(A0, ZERO, T0));              /* done: a0 = a (ret) */
```

测试框架拼装成二进制 → 喂给 `./build/temu -b` → 检查 `halt_ret`。**halt_ret = 55** 就通过。

> **里程碑**：**你的 CPU 现在能算斐波那契**。四周前它还不存在——P0 搭了壳，P1 装了调试器，P2 分配了状态，P3 让它会动。你可以 `si 1` 单步看 `$a0` 从 10 慢慢减到 0；可以 `w $t1` 看 b 每一轮被更新；可以 `x 10 $pc` 看自己刚编码的指令。

`sum(1..10)` 更短：

```c
RUN("sum(1..10) = 55", 55,
    ADDI(T0, ZERO, 10),              /* i = 10 */
    ADDI(A0, ZERO, 0),                /* s = 0  */
    ADD(A0, A0, T0),                  /* loop: s += i */
    ADDI(T0, T0, -1),
    BNE(T0, ZERO, -8));               /* if i != 0, back to loop */
```

5 条指令搞定 1-10 求和。`halt_ret = 55`。

---

## 11. 理论视角：RISC vs CISC——1980 到 2025

我们花了大量篇幅在 RV32I 上。作为章节的历史收尾，值得看看**为什么是 RISC**。

### 11.1 1980 年的起点

Patterson 和 Ditzel 1980 年在 ACM *Computer Architecture News* 发表 *The Case for the Reduced Instruction Set Computer*——现代 RISC 运动的起点。核心论点：

1. **复杂指令很少被用到**——编译器 emit 的常用指令就十几条
2. **简单指令更容易做快**（流水线友好、固定长度并行译码）
3. **省下的译码硅面积**可以换更多寄存器、更大 cache、更高频率
4. **固定长度** → 并行译码 → 更高 IPC（instructions per cycle）

当时的 CISC 代表（VAX）有一条 `POLY` 指令：按给定系数表求多项式的值。写在 ISA 里，用它的编译器**几乎没有**。设计一条几百万晶体管都没人用的指令——这就是 Patterson & Ditzel 的"Case"。

### 11.2 然后发生了什么

| 年代 | RISC 代表 | CISC 代表 | 裁判 |
|------|----------|-----------|------|
| 1985 | MIPS, SPARC | x86, VAX | 工作站市场偏 RISC |
| 1995 | PowerPC, Alpha | Pentium Pro | 桌面 x86 赢，服务器分庭抗礼 |
| 2005 | ARM 移动端起势 | x86-64 | Intel "CISC 前端 + RISC 后端"（P6 微架构起）|
| 2015 | ARM 移动 | x86 桌面+服务器 | 两极化 |
| 2025 | **ARM + RISC-V 反攻** | x86 市占下降 | Apple Silicon / AWS Graviton / RISC-V 数据中心起 |

关键转折：**1995 年后的 x86 内部就是 RISC 了**。Pentium Pro（P6 微架构）首创"x86 前端译码成 μop（micro-operation），RISC-like 后端执行 μop"——所谓 CISC vs RISC 的战争在芯片**内部**早就结束了，外部 ISA 兼容性只是用户可见的一层皮。

ARM 自己也承认了：**AArch64（2011 发布）**几乎抄了 RISC 的每条规则——固定 32 位指令、31 个 GPR、去掉条件执行、PC 独立出 GPR。**RISC 赢得如此彻底，以至于"CISC" 这个词不再有人严肃使用**。

### 11.3 对你意味着什么

你刚刚在这章里**用约 200 行 C 写了 37 条完整指令**。写一个 x86 base ISA 模拟器（甚至不考虑 SSE/AVX）**要几千行**，原因：

- 变长编码（1-15 字节）
- 6 种可选 prefix（`REX`、操作数大小覆盖、段覆盖...）
- ModR/M 字节 + SIB 字节 + 8/16/32/64-bit 立即数的所有组合
- 几十条为 8086 兼容性保留的老指令

**ISA 复杂度是一个税**——每个实现都要交。今天 Apple / Google / AWS / SiFive 选 RISC-derived 架构，不是因为 RISC "更正确"，是因为**"不需要交那个税"**。

> **核心洞察**：**"RISC vs CISC" 今天不再是技术辩论，是一种文化差异**。你本章写的每一行都站在 RISC 这一边——不是因为它"赢了",而是因为它让你能**在一章里写完**。

---

## 12. 踩坑清单

### 12.1 PC 更新绕过 `s->dnpc` 直接写 `cpu.pc`

INSTPAT body 里写 `cpu.pc = target` 会让"fall-through 免费"的设计崩盘——`cpu_exec` 循环原本在 `exec_once` 后**统一**把 `cpu.pc = s->dnpc`，如果分支 body 提前覆盖 `cpu.pc`，非分支指令还是会被 `cpu.pc = s->dnpc = s->snpc` 吃掉跳转结果。**规则**：分支 / 跳转只改 `s->dnpc`，主循环负责写回。

### 12.2 每条 INSTPAT 都 guard `if (rd != 0)`

这是 §8 的反例。正确做法是 `cpu_exec` 循环末尾一次 `cpu.gpr[0] = 0`。每条 guard 的代价是 37 个重复模板 + 指令 body 不再"看起来像 ISA 伪码"。

### 12.3 INSTPAT 模式顺序反了

**SUB 必须在 ADD 前**——两者 funct3 相同，靠 funct7 区分，pattern match 是线性扫描，第一条命中即停。同理 SRA/SRL、SRAI/SRLI。**catch-all `invalid`** 必须在表**最末**，否则会吞掉所有指令。见 §6.2。

### 12.4 JAL 的 link 值用 `s->pc` 而非 `s->snpc`

`s->pc` 是当前指令地址，`s->snpc` 才是"下一条"（= pc + 4）。用 `s->pc` 做 link → `ret` 会跳回 JAL 自身 → 死循环。这是 PA 社区反复重现的 bug。

### 12.5 LB/LH 符号扩展 vs LBU/LHU 零扩展

`LB` 读字节后按有符号扩展成 32 位；`LBU` 零扩展。混淆会让 `0xFF` 从 `-1` 变成 `255` 或反过来——字符串处理（`strlen` 探测 `\0`）、negative offset 解码、checksum 都会崩。

### 12.6 B-type 偏移是字节，不是指令数

`BEQ rs1, rs2, 8` 跳 2 条指令（2 × 4 字节），不是 8 条。RV32 全部跳转偏移以字节为单位。容易在手写测试汇编时写错。

### 12.7 B-type / J-type 的 bit 0 隐式为 0

立即数编码时 bit 0 **不出现在指令字**里——RISC-V 规定指令地址 2 字节对齐（为未来 C 扩展留空），所以 bit 0 永远是 0，编码里直接省掉。SEXT 拼立即数时记得左移补 0。

### 12.8 SRAI / SRA 没做 `(sword_t)` 强转

C 里 `unsigned >> n` 是**逻辑右移**（补零），只有 `signed >> n` 才是**算术右移**（补符号位）——后者还在 C89/C99 是 UB / implementation-defined，直到 C23 才落定。必须 `(word_t)((sword_t)src1 >> n)`。

### 12.9 移位量没 mask 低 5 位

`src1 << src2` 当 `src2 >= 32` 时 C 是 UB。RV32 硬件只看 `src2 & 31`——模拟器必须显式 mask，不能依赖 C 的未定义行为跟硬件"碰巧一致"。

### 12.10 JALR 目标地址漏了 `& ~1`

Spec 规定 JALR 目标最低位强制清零（为 RVC 压缩指令留扩展空间）。不做这一步，未来接入 C 扩展时所有 `jalr` 都会跳到奇数地址崩盘。

### 12.11 invalid 指令直接 `panic` 而非 `temu_set_abort`

`panic` 直接 abort 进程，P1 调试器没机会检查现场；应走 `TEMU_ABORT` 软停机路径，让 `info r` / `x` 还能用。

### 12.12 EBREAK 和 ECALL 搞混

两者 opcode 都是 `1110011`，funct3 全 0，rs1/rd 全 0——**只差 imm 位**：`ECALL = 0`、`EBREAK = 1`。INSTPAT pattern 写错一位就完全调不起来，症状是 `ebreak` 被当 `ecall` 或反过来。

---

## 13. 动手练习

### Easy 1 · 伪指令反汇编

`disasm.c` 里加规则：`ADDI rd, zero, imm` 打印成 `li rd, imm`；`ADDI rd, rs1, 0` 打印成 `mv rd, rs1`；`JAL x0, offset` 打印成 `j offset`。**只改显示，不动执行**。

**学到**：伪指令是**汇编器/反汇编器的约定**，不是 ISA 的一部分。

### Easy 2 · `--itrace=N` 配置 ring buffer 大小

命令行选项让你指定 ring buffer 大小，默认 16。需要从 static 数组改成 `malloc`。

**学到**：CLI 配置 + 动态分配替代静态数组的模式。

### Medium 1 · `info p` 命令

新增 REPL 命令 `info p` 打印 `g_last_disasm` 的所有字段（name/type/inst/pc/rd/rs1/rs2/imm）。整合进 P1 的 `info r / info w` 家族。

**学到**：调试即特性；复用已有的 struct；P0 表驱动命令分派的模式。

### Medium 2 · `si N` 单步时打印反汇编

改 `cmd_si` 让每步打印 pc + disasm。单纯改输出，不动 CPU 逻辑。

**学到**：执行和追踪的交织；什么时机调 `disasm()`（要在 exec_once 之后，因为 `g_last_disasm` 是它填的）。

### Medium 3 · 手写 `strlen` 汇编测试

在 `0x80001000` 预置字符串（用 SB 一字节一字节写），然后循环 LB + BEQ 计数直到遇到 `\0`，结果放 a0。

**学到**：循环 + 访存的组合；真正的"程序"是这些小指令堆出来的。

### Hard 1 · 实现 M 扩展（乘除）

加 8 条 INSTPAT：MUL, MULH, MULHU, MULHSU, DIV, DIVU, REM, REMU。

**学到**：
- MULH 要 `(int64_t)(int32_t)rs1 * (int64_t)(int32_t)rs2 >> 32`——64 位中间结果
- RISC-V 的除零**不陷入异常**，返回 `-1`（DIV）或 `rs1`（REM）——spec 的意外决定，要按 spec 写
- 签名/无签名的三种乘法为什么都要：编译器做 64 位乘法时要用到

### Hard 2 · 独立反汇编工具 `./temu-disasm image.bin`

复用 `pattern_match` + `decode_operand` + `disasm()`——**不跑 CPU**，只打印。

**学到**：译码路径可以**独立于执行**复用；这正是 objdump 的核心。

### Hard 3 · 递归 fib(6) = 8

用正确的调用约定：sp 作为栈指针、ra 保存返回地址、栈帧保存/恢复。

**学到**：栈纪律、寄存器保存、函数调用如何由 jump + load + store 组合而成。这是"理解 C 调用"的必经之路。

---

## 14. 本章小结

**你应该能做到**：

- [ ] 画出 RV32I 六种编码格式的字段布局
- [ ] 写出 `SEXT` 的左移-算术右移实现并解释原理
- [ ] 写出 `immB` 的位重组（B-type 立即数的拆分规则）
- [ ] 解释 `pattern_match` 的 `?`/`0`/`1` 如何变成 `(inst & mask) == key`
- [ ] 解释为什么 PC 更新走 `Decode{snpc, dnpc}` 而不是 `cpu.pc += 4`
- [ ] 写出 JAL 的 link 捕获顺序（为什么先存 `link` 再改 `dnpc`）
- [ ] 用 `isa-encoder.h` 手写一个 fib(10) 的测试
- [ ] 解释 x0 硬连线为什么在 `cpu_exec` 循环末尾实现

**你应该能解释**：

- [ ] ISA 为什么是"契约"而不是"实现"——举 Intel time machine 为例
- [ ] 为什么 B-type 立即数在指令字里是"乱的"——关联到硅面积和 PLA 并行性
- [ ] 为什么我们的模式匹配和硬件 PLA 是同一抽象的两种实现
- [ ] RISC vs CISC 辩论 1980 的起点、今天的现状、你选 RV32I 的原因
- [ ] 为什么 `lb` 符号扩展、`lbu` 零扩展——以及 P2 的 `word_t ret = 0` 怎么让零扩展免费
- [ ] 为什么分支指令的 `dnpc` 只在条件成立时写入

---

## 15. 延伸阅读

- **RISC-V Unprivileged ISA Spec, Chapters 2 & 19** — base ISA + instruction listing。Chapter 2 是本章的"圣经"，15 页可以一口气读完
- **Patterson & Ditzel · *The Case for the RISC*（ACM CAN 1980）** — 现代 RISC 的起点，8 页，今天读仍然 fresh
- **Hennessy & Patterson · *Computer Architecture: A Quantitative Approach*, Appendix A** — ISA 设计准则的形式化讨论
- **Hennessy & Patterson · *Computer Organization and Design: RISC-V Edition*, Chapter 2** — 同两位作者写给本科生的版本，和我们的实现并行阅读
- **Shen & Lipasti · *Modern Processor Design*, Chapter 4** — 真 CPU 的译码流水线；PLA vs 微码 ROM 的 tradeoff
- **riscv-isa-manual on GitHub** — spec 的 LaTeX 源码。*"Base Integer Instruction Set, RV32I"* 只有 15 页
- **Jim Keller 的访谈**（Lex Fridman / Anandtech） — Alpha / K7/K8 / Apple A4-A5 / Zen / 现在的 RISC-V 首席设计师。听他讲"为什么 ISA 不如微架构重要"，作为对本章"RISC 万岁"的平衡

---

## 与后续章节的连接

| 下一章 | 本章埋下的伏笔 |
|--------|---------------|
| P4 差分测试 | `isa_exec_once` 是对拍单位；`g_last_disasm` 直接喂 difftest 的错误报告 |
| P5 I/O 设备 | `lw/sw` 路径自动覆盖 MMIO 地址——P2 留的 `mmio_access` stub 填实 |
| P6a 异常 + CSR | INSTPAT 表的 `1110011` opcode 槽位预留给 ECALL / EBREAK-trap / MRET / CSRR*；`trap_pending` hook 已经在 exec_once 末尾 |
| P6b 虚拟内存 | `Decode.pc` 是 vaddr；`paddr_read` 上方加一层 `vaddr_read`，取指和访存都走翻译 |
| 全书 | `temu_set_end(s->pc, R(10))` 把 a0 吐到 host 的 exit code——是未来 "RV32I 上跑真程序" 的交接点 |

本章之后 CPU 是"真的 CPU"了。P4 开始我们会**怀疑自己写错**——差分测试的主题就是怎么证明自己对。

**下一站：P4——让另一个 CPU 来检查我们的 CPU。**
