---
title: "P6a · 异常与中断：CPU 第一次学会放下手头的事"
date: 2026-07-24
categories: [计算机系统]
tags: [Trap, Interrupt, CSR, Zicsr, mstatus, mtvec, mepc, mret, Privileged-Spec, RV32I]
math: true
---

## 本章目标

读完本章你能回答：

- 为什么"外部事件打断 CPU"必须是 **硬件机制**，不能只靠软件轮询？
- RISC-V 的 `mstatus / mtvec / mepc / mcause` 是什么组织形式，为什么不塞进 32 个 GPR 里？
- 一条 `csrrw` 指令的 read-modify-write 为什么必须**原子**——它跟 `lw + addi + sw` 三条组合比有什么本质区别？
- 同步异常（`ecall`）和异步中断（定时器）在 `mepc` 上**语义不同**，为什么？
- `--ebreak=halt` 和 `--ebreak=trap` 这两个模式是什么、为什么要共存？
- 两套 CSR 实现怎么对拍？`mip.MTIP` 为什么必须从对拍里剔除？

**动手做到**：

- 读 `info c` 查看 7 个 M-mode CSR 的当前值
- 用 `csrrw t0, mtvec, t1` 手动改 mtvec，然后 `ecall` 陷入自定义 handler
- 跑通 `tests/isa/isa.c` 里的 `test_trap`（4 条 trap round-trip）和 `test_interrupt`（timer 打断 3 次后退出）
- 给 `--ebreak=trap` 跑旧测试、观察它们怎么**因为 ebreak 不再 halt** 而死循环（对比 halt 模式）

---

## 1. 问题动机：顺序执行的 CPU 是"聋瞎的"

到 P5 结束，TEMU 已经能：

- 执行 RV32I 全部 37 条指令（P3）
- 从串口输出字符（P5 §6）
- 读定时器看"时间过了多少"（P5 §7）

但它还**做不到一件事**：让外部事件**主动打断** CPU 的执行流。

举两个具体例子：

**例 1：串口输入**。假设 host 键盘按下一个键，UART 硬件收到一个字节。CPU 不知道——它还在按部就班跑主循环。程序要看到这个字节只有两条路：

- **轮询**：主循环里每隔几条指令 `lb t0, 0(serial)`，看有没有新数据
- **中断**：让 UART **主动告诉** CPU "有新字节了"，CPU 暂停手头的事去处理

**例 2：时间片调度**。OS 内核想让用户进程每跑 10ms 就切一次。如果只能靠用户进程"自愿归还" CPU（协作式调度），一个死循环就能把整个系统卡死。要做**抢占式**调度，必须有"定时器到点了硬件强制 CPU 跳走"的能力。

轮询和中断的**本质差异**：

| 维度 | 轮询 | 中断 |
|--|--|--|
| 谁发起 | 软件主动 | 硬件被动 |
| 响应延迟 | 最坏 = 主循环周期 | 几条指令（近乎即时）|
| CPU 占用 | 即使没事也要查 | 空闲时真空闲 |
| 代码结构 | 主循环里到处塞 `if (device_ready) ...` | handler 函数独立 |
| OS 隔离 | 用户程序可以"不检查"→ 不可靠 | 用户程序**想躲也躲不掉** |

> **核心洞察**：**操作系统的存在前提是"可以打断用户程序"**。没有这一条，kernel 只是一个用户调用的库——想啥时候回 kernel 是用户说了算，用户不回就回不了。Trap 机制是内核拿回控制权的**唯一合法通道**。

**顺序执行的 CPU 完不成的三件事**：

1. **抢占式多任务**：不能强制切换用户进程
2. **系统调用**：`printf` 怎么从用户态进内核？只能靠"主动触发一次陷入"
3. **异常处理**：除零、非法指令、缺页——这些必须有"CPU 自己放弃当前 PC 跳去处理"的通道

P6a 的目标就是把这三件事**同一套机制**一次性做出来。

---

## 2. 核心洞察：trap = 硬件发起的"函数调用"

"函数调用"你熟悉：

```
call f:
    push return_pc     ; 记住回来的地方
    jmp  f             ; 跳过去
ret:
    pop  return_pc
    jmp  return_pc     ; 回来
```

`trap` 是一个**特殊的函数调用**——**不是 CPU 主动调的，是被迫的**：

```
trap:
    mepc    := pc          ; "函数地址" 自动存到 CSR
    mcause  := cause       ; 附带"为什么被打断"的原因码
    mstatus := save_MIE    ; 把中断使能状态压栈（准备关中断）
    pc      := mtvec       ; 跳到预先设定的"handler"
mret:
    pc      := mepc        ; 从 CSR 读回来的地址
    mstatus := restore_MIE ; 恢复中断使能
```

**对照关系**：

| 函数调用 | Trap |
|--|--|
| `call f` 软件主动 | 硬件被动（中断 / 异常发生）|
| return_pc → 栈 | mepc → CSR |
| 跳到 f（已知）| 跳到 mtvec（OS 预先登记）|
| ret 弹栈回原地 | mret 从 mepc 回原地 |
| 一个 GPR 保存 x1/ra | 一个 CSR 保存 mepc |

**⚠️ 常见误解**：以为 trap 是"完全不同的机制"。不是——**trap 就是"函数调用 + 几个额外的 CSR 簿记 + 硬件主动性"**。学过函数调用再学 trap 只是多记几个名字。

**为什么 TEMU 必须建模这一套**：

1. **语义忠实**：真硬件就这样，模拟器偷懒走别的路径 → guest 程序在真芯片上跑起来行为不一样
2. **统一处理**：同步异常（除零、非法指令、ecall 系统调用）、异步中断（定时器、外设）**全都走这一条通道** —— CPU 用 `mcause` 区分类型
3. **为 OS 铺路**：stage 6b 的 U-mode、stage 6d 的 xv6 移植，全部依赖这一套

下面的实现细节拆成 5 个 chunk：CSR 文件（§3）、CSR 指令（§4）、trap 机制（§5）、中断 delivery（§6）、difftest 扩展（§7）。

---

## 3. CSR 寄存器文件：GPR 之外的第二套寄存器

### 3.1 为什么 CSR 不塞进 32 个 GPR

一个朴素的问题：`mstatus / mtvec / mepc / ...` 为什么不直接复用 `x16, x17, x18, ...`？

三个硬性理由：

**(1) 访问权限分层**

GPR 任何指令都能读写（包括用户态）。CSR 在真硬件里要按**特权级**过滤：

- `mstatus`、`mtvec`：只有 M-mode 能访问；U-mode 程序读它们触发 illegal instruction
- `cycle`（循环计数）：U-mode 可读
- `satp`（页表基址）：只 S-mode 及以上可访问

GPR 没有这种分层。硬塞进 GPR 会让"用户程序读 x16 有时 OK 有时陷入"——语义混乱。

**(2) 数量 vs 使用频率的错配**

RV32I 有 32 个 GPR，每条指令都用几个。CSR 有 **4096 个编号**（12 位 addr），大多数指令完全不碰它们——只有 6 条 Zicsr 指令专门访问。两种寄存器在**编码成本**上不同档：

- GPR 编号 5 bit，每条 R-type 指令塞两三个名字
- CSR 编号 12 bit，独占一条指令的 `imm` 字段

塞一起就得选择：要么 GPR 扩到 4096 个（浪费），要么 CSR 压到 32 个（不够用）。

**(3) 语义副作用**

某些 CSR **读/写**本身就是副作用：

- 写 `mtimecmp` 改变下次中断触发时间（TEMU 走 MMIO，但有些 ISA 用 CSR）
- 写 `satp` 切换页表（S-mode）→ TLB flush
- 读 `cycle` 返回一个"正在变"的值

GPR 的读写语义是纯粹的"数据流"。混进 CSR 这种"带行为的寄存器"会让流水线调度、乱序执行的推理复杂一个量级。

**结论**：CSR 是**独立的地址空间**，逻辑上像 MMIO 的缩影——"挂在某个编号上、读写有意义、需要特殊指令访问"。

### 3.2 表驱动实现：一个数组加 4 个函数

P2 的 GPR 用数组 `cpu.gpr[32]`，编号即下标。CSR 不能——12 位 addr 意味着 `word_t csr_arr[4096]` 浪费 16KB 存 **7 个真实值**。要么**稀疏表**，要么**switch-case**。

TEMU 选稀疏表（`src/isa/riscv32/csr.c:14`）：

```c
typedef struct {
    uint32_t    addr;
    const char *name;
    word_t     *field;
} csr_entry_t;

static csr_entry_t csr_table[] = {
    { CSR_MSTATUS,  "mstatus",  &csr.mstatus  },
    { CSR_MIE,      "mie",      &csr.mie      },
    { CSR_MTVEC,    "mtvec",    &csr.mtvec    },
    { CSR_MSCRATCH, "mscratch", &csr.mscratch },
    { CSR_MEPC,     "mepc",     &csr.mepc     },
    { CSR_MCAUSE,   "mcause",   &csr.mcause   },
    { CSR_MIP,      "mip",      &csr.mip      },
};
```

**三个角色合一**：

| 字段 | 用途 |
|--|--|
| `addr` | Zicsr 指令译码后的 12-bit 编号 |
| `name` | `info c` 打印 / `p $mtvec` 求值 / 反汇编 |
| `field` | 指向真实存储位置的指针 |

4 个函数全部走这张表：

```c
word_t csr_read(uint32_t addr);          /* Zicsr 指令 */
void   csr_write(uint32_t addr, word_t); /* Zicsr 指令 */
const char *csr_name(uint32_t addr);     /* 反汇编 */
bool   csr_lookup(const char *name, word_t *out);  /* expr 求值 */
```

**为什么值得表驱动**：

- 加一个 CSR = 加一行。stage 6b 加 `sstatus / sepc / stvec / satp` 时不改 4 个函数
- 编号 / 名字 / 存储位置**单一真实源**——不会出现"csr_read 认 0x305 但 csr_name 不认"的 bug
- `info c` 和 `p $mtvec` 复用同一张表 → 调试器和运行时永远一致

**7 个 M-mode CSR 的语义**（Privileged Spec v1.12）：

| 编号 | 名字 | 语义 |
|--|--|--|
| 0x300 | `mstatus` | 全局状态（MIE / MPIE / MPP 位）|
| 0x304 | `mie` | 中断源屏蔽（MTIE = bit 7 = 定时器中断使能）|
| 0x305 | `mtvec` | trap handler 入口地址 + 模式位 |
| 0x340 | `mscratch` | handler 专用临时寄存器（保存 sp 等）|
| 0x341 | `mepc` | trap 时的返回 PC |
| 0x342 | `mcause` | trap 原因（bit 31 = 中断 vs 异常；低位 = 具体 cause 码）|
| 0x344 | `mip` | 中断 pending 位（MTIP = bit 7）|

编号**不连续**（0x300 – 0x305, 0x340 – 0x344）不是 TEMU 想省——是 spec 故意把 M-mode 常用位聚到 0x30x，把"trap 上下文"聚到 0x34x，为扩展预留中间段。

### 3.3 未实现的 CSR 号：读零写丢

`csr_read(0x7c0)` 返回什么？Spec 允许两种合法行为：

1. **读零写丢**：找不到就返回 0；写忽略
2. **非法指令 trap**：没见过的 CSR 号 → illegal instruction exception

TEMU stage 6a 选 (1)（`csr.c:38`）：

```c
word_t csr_read(uint32_t addr) {
    csr_entry_t *e = find_by_addr(addr);
    return e ? *e->field : 0;
}
```

**理由**：真实 OS 启动代码会"探测"可选 CSR（比如 `cycleh`、性能计数器），探到不存在就降级。这种 **feature probing** 如果每次都 trap 就太吵——linux 启动日志会刷几十行 illegal instruction。

**代价**：程序写错 CSR 号（比如想写 `mtvec=0x305` 打成 `0x350`）不会被检测。stage 6a 靠 difftest 的两实现对拍部分弥补——两边都"读零写丢"一致，只有一边写错才暴露。

**tests/isa/isa.c:452**：

```c
/* Unknown CSR number — stage 6a policy is read-zero / write-drop */
TEST("unknown csr reads zero", 0,
     ADDI(T0, ZERO, 0xff),
     CSRRW(ZERO, 0x7c0, T0),
     CSRRS(A0, 0x7c0, ZERO));
```

**与 stage 6b 的衔接**：6b 会引入特权级，那时非法 CSR 访问必须 trap（spec 强制）。但 stage 6a 保持宽松，等真正有特权区分时再收紧。**theoretical ideal 让位给教学 pragmatic**。

### 3.4 `info c` 与表达式求值

调试器端的两个接口（`src/monitor/sdb.c`）：

**（1）`info c` 打印所有 CSR**：

```
(temu) info c
  mstatus  (0x300) = 0x00000000
  mie      (0x304) = 0x00000000
  mtvec    (0x305) = 0x80000004
  mscratch (0x340) = 0x00000000
  mepc     (0x341) = 0x80000024
  mcause   (0x342) = 0x0000000b
  mip      (0x344) = 0x00000000
```

**（2）表达式求值扩展**：

P1 的 expr 求值器原本只认 `$pc / $x0 / $ra ...` GPR 名字。6a-1 扩展到 CSR：

```
(temu) p $mtvec
$1 = 0x80000004
(temu) p $mepc + 4
$2 = 0x80000028
```

实现上就是 `isa_reg_val("mtvec")` 先查 GPR 表，找不到再 `csr_lookup("mtvec", &val)`。**零工程量**——因为 P1 当初设计 `isa_reg_val` 时留了扩展口。

> **连接**：这是 **P1 埋的伏笔 P6a 兑现** 的又一例。类比 P4 的 `paddr_touched_mmio`——早声明、晚使用、一次到位。你会发现整个 TEMU 的结构里这种"跨章节接缝"反复出现，是值得注意的工程品味。

---

## 4. Zicsr 指令族：6 条 read-modify-write

RV32I 本身不含 CSR 指令——它们在 **Zicsr 扩展**里（Z 代表"extension"，i 代表 integer，csr 是缩写）。Spec 把这 6 条单列成扩展是因为早期 embedded 核（无 OS、无中断）可以彻底省略 CSR。今天 99% 的实现都带 Zicsr，所以习惯上不再强调"扩展"。

### 4.1 六条指令的统一模板

所有 6 条都是 **read-modify-write** 原子操作：

```
old_val = csr_read(addr)
new_val = f(old_val, operand)
csr_write(addr, new_val)
rd      = old_val        ; 注意返回 "旧值" 不是 "新值"
```

三种 modify 语义 × 两种 operand 来源 = 6 条：

| 指令 | `f(old, op)` | operand 来源 |
|--|--|--|
| `csrrw`  | `op`            | rs1（寄存器）|
| `csrrs`  | `old \| op`     | rs1 |
| `csrrc`  | `old & ~op`     | rs1 |
| `csrrwi` | `op`            | imm（5 位 zero-extend）|
| `csrrsi` | `old \| op`     | imm |
| `csrrci` | `old & ~op`     | imm |

- `csrrw`：**whole-write**（覆盖）
- `csrrs`：**set bits**（按位或；常用于"使能某功能"）
- `csrrc`：**clear bits**（按位与非；常用于"关闭某功能"）

**INSTPAT 实现**（`inst.c:227`）：

```c
INSTPAT("??????? ????? ????? 001 ????? 1110011", "csrrw",  I, {
    uint32_t addr = (uint32_t)BITS(inst, 31, 20);   /* 12-bit CSR 编号 */
    word_t   old  = csr_read(addr);                  /* 先读 */
    csr_write(addr, src1);                           /* 再写 */
    R(rd) = old;                                     /* rd 拿旧值 */
});
```

**关键细节**：`addr` 不是 sign-extended imm，**就是 12 位无符号 CSR 编号**——直接 `BITS(inst, 31, 20)` 不经过 SEXT。这是 Zicsr 在编码上和 I-type 的**唯一区别**。

### 4.2 rs1 = 0 的豁免规则

Spec §9.1.1 隐藏规则：

> "If rs1=x0 (or zimm=0), the instruction shall not write to the CSR."

为什么要这条？考虑场景：你只想**读** `mcause`，不想改。朴素写法：

```asm
csrrs a0, mcause, zero    ; a0 = mcause; mcause |= 0
```

按朴素语义，`mcause |= 0` 是空操作——但硬件不知道"`|= 0` 不改值"。它依然会做一次 write cycle。**问题**：某些 CSR **写入本身有副作用**（清中断标志、触发计数器复位）。Spec 的豁免让编译器可以放心用 `csrrs rd, csr, x0` 作为**纯读**。

TEMU `inst.c:236`：

```c
INSTPAT("??????? ????? ????? 010 ????? 1110011", "csrrs",  I, {
    uint32_t addr = (uint32_t)BITS(inst, 31, 20);
    word_t   old  = csr_read(addr);
    if (BITS(inst, 19, 15) != 0) csr_write(addr, old | src1);
    R(rd) = old;
});
```

`BITS(inst, 19, 15)` 是原始 rs1 编码位——**不是 `src1` 的值**。这一点关键：`src1` 可能正好等于 0（寄存器里就是 0），但那不触发豁免；豁免看的是**指令字里 rs1 字段写了 0 号寄存器**。

**⚠️ 常见误解**：以为"src1 == 0 时不写"。错——spec 是看编码位 `rs1 == 0`。即使 `x5 = 0`，`csrrs rd, csr, x5` 仍然执行 write（因为写的是 `old | 0 = old`，没副作用情况下等价但规范严格区分）。

**csrrw 不享有豁免**：`csrrw rd, csr, x0` 会**真的** CSR := 0。这让它成为"强制清零"的惯用法。

### 4.3 原子性与 aliasing 陷阱

`csrrw t0, mscratch, t0` 这种写法 rd 和 rs1 都是 t0。spec 要求 rd 得**旧值**，不是新值。TEMU 实现如何保证？

```c
word_t old  = csr_read(addr);   /* ← 先读 */
csr_write(addr, src1);          /*   再写，此时 src1 是 t0 的 _当前_ 值 */
R(rd) = old;                    /*   rd = 旧值 */
```

`src1` 在 INSTPAT 展开时已经是 `cpu.gpr[rs1]` 的快照（`decode_operand_I` 在 body 前提取），不会被后面 `R(rd) = ...` 修改影响。这是 P3 §3.1 的 Decode 结构体设计的回报。

**对应测试**（`tests/isa/isa.c:395`）：

```c
TEST("csrrw aliased rd=rs1", 0x55u,
     ADDI(T0, ZERO, 0x55),
     CSRRW(ZERO, CSR_MSCRATCH, T0),       /* mscratch = 0x55 */
     ADDI(T0, ZERO, 0x66),
     CSRRW(T0, CSR_MSCRATCH, T0),         /* t0 <- 0x55; mscratch <- 0x66 */
);
```

**原子性为什么重要**：朴素拆 `csrrw` 为 `lw + mv + csrw` 三条指令不行——真硬件上中间可能被**中断**打断，读到的"old"和写下去的"new"之间 CSR 已经被 handler 改过。Zicsr 是 spec 级别的原子，硬件保证"这三个操作之间不可能插入任何事件"。

TEMU 单线程模拟器这件事是**碰巧对的**（我们的 `isa_exec_once` 就是一次跑完不会被任何东西打断），但代码**必须**按"原子"的心智模型写——这样搬到多核 / 真硬件才对。

---

## 5. Trap 机制：同步异常

CSR 搭好了，Zicsr 指令能读写它们。现在实现"硬件主动改 PC + 动 CSR"的 **trap** 机制。

### 5.1 staging vs commit：为什么拆成两步

P3 §3.2 定下的铁律：**INSTPAT body 只写 `s->dnpc`，cpu_exec 循环末尾统一落到 `cpu.pc`**。这个不变量保护了"fall-through 免费"和"分支不提前越权"。

`ecall` 要改 `cpu.pc = mtvec`。如果 INSTPAT body 直接写 `cpu.pc`，就破坏了 P3 的不变量。两种解法：

| 方案 | 利 | 弊 |
|--|--|--|
| A. INSTPAT 的 `trap_take` 只把 "准备 trap" 这件事记下来；cpu_exec 末尾 commit | 保持 INSTPAT body 纯净 | 多一层 pending 状态 |
| B. 改 `trap_take(Decode *s, ...)`，通过 `s->dnpc` 传 PC 变化 | 直接 | 3 条指令的 body 要传 `s` |

stage 6a 选 **A**（`trap.c:14`）——更重要的理由是**定时器中断也要改 PC**，而定时器中断**不在任何 INSTPAT 里**，它在 cpu_exec 末尾触发。A 方案让两条路径走**同一个 commit 点**。

**staging slot**：

```c
static struct {
    bool   active;
    word_t cause;
    word_t tval;
    word_t epc;
} pending;

void trap_take(word_t cause, word_t tval, word_t epc) {
    Assert(!pending.active, "trap_take: trap already pending ...");
    pending.active = true;
    pending.cause  = cause;
    pending.tval   = tval;
    pending.epc    = epc;
}
```

**为什么 `Assert(!pending.active)`**：stage 6a 保证"一条指令最多触发一次 trap"。如果 ecall body 里调了 `trap_take`、又在同一条指令里触发定时器（理论上不可能，因为我们顺序执行）——这个 assert 会立刻拍你醒。**不变量显式化**是 TEMU 一贯品味：重要规则用 assert 写进代码，不靠注释。

### 5.2 trap_commit：真正落到 CSR

INSTPAT 返回后，cpu_exec 检查 `trap_pending()`，若为真则 `trap_commit()`（`trap.c:33`）：

```c
void trap_commit(void) {
    csr.mepc   = pending.epc;
    csr.mcause = pending.cause;

    /* mstatus transitions on trap entry:
     *   MPIE <- MIE
     *   MIE  <- 0
     *   MPP  <- current privilege (always M in stage 6a) */
    word_t s   = csr.mstatus;
    word_t mie = (s >> MSTATUS_MIE_BIT) & 1u;
    s = (s & ~(1u << MSTATUS_MPIE_BIT)) | (mie << MSTATUS_MPIE_BIT);
    s &= ~(1u << MSTATUS_MIE_BIT);
    s = (s & ~MSTATUS_MPP_MASK) | (PRIV_M << MSTATUS_MPP_LSB);
    csr.mstatus = s;

    cpu.pc = csr.mtvec & ~3u;   /* direct mode */
    pending.active = false;
}
```

逐条看：

**（1）`csr.mepc = pending.epc`**：被打断的 PC 进 CSR。这是 `mret` 回去的地方。

**（2）`csr.mcause = pending.cause`**：原因码。Handler 读它分派——"是 ecall 还是定时器还是 illegal inst"。

**（3）mstatus 的 MIE/MPIE/MPP 簿记**：下一节专讲。

**（4）`cpu.pc = csr.mtvec & ~3u`**：跳到 handler 入口，mask 掉低 2 位（见 §5.6 mtvec 模式）。

**注意这里直接改 `cpu.pc`**——不是 `s->dnpc`。因为此时已经在 `isa_exec_once` 之外，`cpu.pc = s->dnpc` 已经执行过了，我们现在**覆盖**它。这是合法的：P3 的不变量是"INSTPAT body 不写 cpu.pc"，trap_commit 在 body 之外，不受约束。

### 5.3 mstatus.MIE / MPIE 的栈式语义

`mstatus` 有两个**中断使能**相关位：

- `MIE`（bit 3）：当前是否允许中断
- `MPIE`（bit 7）：上一次 trap 前 MIE 的值（"Previous"）

**Trap 发生时**：

```
MPIE <- MIE       (保存旧的 MIE 到 MPIE)
MIE  <- 0         (关中断，handler 跑在中断禁用下)
```

**mret 执行时**：

```
MIE  <- MPIE      (恢复)
MPIE <- 1         (重置为默认)
```

**⚠️ 常见误解**：以为这只是"一对 bit 帮忙记值"。不——它**实质上是一个深度为 1 的栈**。为什么必须关中断？

**情景**：handler 跑了一半，又来一个中断。如果 MIE 还是 1，立即再次陷入——**MPIE 被新 trap 覆盖**，前一次 trap 的状态丢了。栈溢出成无限嵌套——叫 **interrupt storm**，真硬件上会导致死机。

所以 spec 规定：trap 一发生，MIE 立刻清零。Handler 跑完前不会再被打断。

**这是个 depth-1 栈**：只能嵌套 1 层。如果你想让 handler 允许中断（比如长跑的 handler 希望被更高优先级中断），就得**手动**：

```asm
; handler 开头
csrrs t0, mstatus, 8    ; 手动重开 MIE
; ... 敏感工作 ...
csrrc t0, mstatus, 8    ; 关回来
mret                    ; 硬件再恢复一次
```

**现代 OS 的做法**：handler 分两段——**上半部（top half）**在关中断下快速登记事件；**下半部（bottom half，deferred work）**开中断慢慢处理。Linux 的 `tasklet` / `softirq` / `workqueue` 就是这一套。

**MPP（Machine Previous Privilege）**：bit 11-12 记录 trap 前的特权级。stage 6a 没有 U-mode，MPP 永远是 M——但代码里已经正确写了，为 stage 6b 铺路。

### 5.4 mret：对称的返回

`mret` 指令（`trap.c:61`）：

```c
void trap_mret(word_t *dnpc) {
    *dnpc = csr.mepc;

    word_t s    = csr.mstatus;
    word_t mpie = (s >> MSTATUS_MPIE_BIT) & 1u;
    s = (s & ~(1u << MSTATUS_MIE_BIT)) | (mpie << MSTATUS_MIE_BIT);
    s |= (1u << MSTATUS_MPIE_BIT);    /* MPIE := 1 reset */
    csr.mstatus = s;
}
```

**注意：写 `*dnpc` 而非 `cpu.pc`**。和 `trap_commit` 对比：

| 触发点 | 改 PC 的方式 |
|--|--|
| `ecall` / `ebreak` INSTPAT | 只 stage，cpu_exec 末尾 `trap_commit` 直接改 `cpu.pc` |
| `mret` INSTPAT | 通过 `s->dnpc` 改——保持 INSTPAT 纯粹 |

为什么非对称？`ecall` 的 pending_trap 机制已经接入 cpu_exec 的两阶段，有 commit 步可以越权；`mret` 是普通 INSTPAT，必须走 dnpc。

**这不是 bug 是设计**——见 trap.h:13 注释：

> "Writing dnpc (not cpu.pc directly) preserves the invariant that INSTPAT bodies produce PC through the Decode channel only."

**验收**（`tests/isa/isa.c:518`）：

```c
TEST("mret restores MIE", 8,            /* bit 3 = 8 */
     /* ... handler 里 CSRRS MIE := 1 前 ECALL ... */
     /* 返回后读 mstatus，检查 MIE 位 == 1 */
);
```

测完 **mret 后 MIE = 1**（恢复）、**MPIE = 1**（重置）。

### 5.5 mepc 是"当前"还是"下一条"？

这是 trap 语义里**最容易搞错**的一点。两类 trap 规则不同：

| Trap 类型 | `mepc` 存什么 | Handler 返回行为 |
|--|--|--|
| **同步异常**（`ecall`、`ebreak`、illegal inst）| **被打断指令的 PC**（= 当前 PC）| Handler 必须手动 `mepc += 4` |
| **异步中断**（timer、软件中断、外部中断）| **下一条待执行指令的 PC** | Handler 直接 `mret`，回到"原本该跑的下一条" |

**为什么同步异常存"当前"？** 比如 ecall 用作系统调用，handler 可能决定：

- 正常返回用户 → `mepc += 4` 跳过 ecall
- 重试（比如被信号打断）→ 不改 mepc，`mret` 回到 ecall 再跑一遍
- 错误退出 → 完全换个 PC

把决定权交给 handler 更灵活。

**为什么异步中断存"下一条"？** 中断是"指令 A 跑完后" / "指令 B 开始前"插进来的。"当前 PC"这时候没有明确含义——A 已经做完、B 还没开始。Spec 规定存 B 的 PC，mret 回去接着跑 B。

TEMU 的实现：

- ecall INSTPAT：`trap_take(CAUSE_ECALL_M, 0, s->pc)` — `s->pc` 是当前指令
- ebreak-trap INSTPAT：`trap_take(CAUSE_BREAKPOINT, s->pc, s->pc)` — 当前
- 定时器中断（cpu_exec 末尾）：`trap_take(CAUSE_INT_MTI, 0, cpu.pc)` — 此时 `cpu.pc = s->dnpc` 已经更新，指向"下一条"

**验收**（`tests/isa/isa.c:502`）：

```c
TEST("ecall saves mepc = pc_of_ecall", 0x80000024u,
     /* ... ecall 在 0x80000024，handler 读 mepc = 0x80000024 确认 */
);
```

和（`isa.c:551`）：

```c
/* Timer interrupt demo: mepc 自动指向下一条，handler 直接 mret 不加 4 */
TEST("timer interrupt fires 3 times", 3, /* ... */);
```

**⚠️ 常见误解**：ecall 的 handler 不加 4 会**死循环**——mret 回到 ecall 本身、再次陷入、handler 再 mret 回来、再 ecall……。这是 PA 社区经典 bug。

### 5.6 mtvec：direct vs vectored mode

`mtvec` 的低 2 位不是地址位，是**模式选择**：

| 低 2 位 | 模式 | 所有 trap 跳到哪 |
|--|--|--|
| `00` | Direct | 全都跳 `mtvec & ~3u` |
| `01` | Vectored | 异常跳 `base`；中断 `i` 跳 `base + 4*i` |
| `10-11` | Reserved | Spec 禁止使用 |

TEMU stage 6a **只支持 direct mode**（`trap.c:56`）：

```c
cpu.pc = csr.mtvec & ~3u;
```

**⚠️ 常见误解**：以为 mtvec 就是地址，忘了 mask 低 2 位。如果 handler 实际地址是 `0x80000004`，软件写 `csrrw mtvec, 0x80000004`（低 2 位是 `00`），mask 后 `= 0x80000004` 巧合对；但换成 `0x80000001` 写入，mask 后是 `0x80000000`——跳错位置。

**为什么不支持 vectored**：

- 教学场景 direct 够用，一个 handler 里 switch-case 分派
- Vectored 需要软件在多个地址各写一份 trampoline，复杂度上升
- Linux / xv6 都走 direct——主流 OS 没用 vectored
- 留给 stage 6b 当扩展练习

---

## 6. 中断 delivery + `--ebreak` 开关

### 6.1 `maybe_take_interrupt` 的位置

同步异常（ecall）在 INSTPAT 里 stage，cpu_exec 末尾 commit。**异步中断**完全不走 INSTPAT——它是 CPU 在指令间隙"发现"有事要处理：

```c
// src/cpu/cpu_exec.c:104
if (g.state == TEMU_RUNNING && (csr.mstatus & MSTATUS_MIE)) {
    if (timer_mtime() >= timer_mtimecmp()) csr.mip |=  MIP_MTIP;
    else                                   csr.mip &= ~MIP_MTIP;

    if ((csr.mip & csr.mie) & MIP_MTIP) {
        trap_take(CAUSE_INT_MTI, 0, cpu.pc);
        trap_commit();
        paddr_touched_mmio = true;   /* let difftest resync */
    }
}
```

**位置关键**：

- 在 `isa_exec_once` **之后** → 当前指令完成
- 在 `cpu.pc = s->dnpc` **之后** → cpu.pc 指向"下一条"（§5.5 异步 trap 存这个）
- 在 `trap_commit`（同步 trap 的 commit）**之后** → 如果 ecall 已经转到 mtvec，中断查的是转跳后的状态（确保不会同一条指令触发两次 trap）
- 在 `difftest_step` **之前** → ref 和 main 同步"见到"这次中断，对拍一致

**三层 gating**：

```
mstatus.MIE      (全局中断使能)
    &
mie.MTIE         (具体中断源使能——定时器)
    &
mip.MTIP         (该中断是否 pending——由墙钟决定)
```

任一为 0 就不 trap。OS 通过 `csrrs/csrrc mstatus, 8`（MIE 位）做**critical section**——进临界区前关中断。

### 6.2 Wall-clock 的精度妥协

`timer_mtime()` 走墙钟 `gettimeofday()`（P5 §7.2）。这让定时器中断的**触发时机不是指令计数驱动**——而是真实时间。

**后果**：

- 跑 TEMU 的机器快 → 中断来得"稀疏"（主循环 1000 条指令之间真时间很短）
- 跑 TEMU 的机器慢 → 中断"密集"
- **不同机器跑同一个 demo 看到的 counter 增速不同**

P5 §7.6 已经讨论过这是 pragmatic 选择（deterministic simulation 的代价）。P6a 的 demo 程序处理方式是"**counter 到 3 就退出**"——不关心多久到 3，只关心真的被打断 3 次。这是**不依赖精度的验收方式**。

**验收**（`tests/isa/isa.c:551`）：

```
arms mtimecmp = 0          → 下次 poll 必触发
handler 里 counter++       → 本次处理完
重设 mtimecmp = 0          → 下次 poll 再触发
直到 counter == 3          → 把 mtimecmp 推到 UINT64_MAX 永不再触
main loop while (counter < 3) 退出
```

不管 TEMU 跑多快多慢，这个 demo 都会稳定地**中断 3 次退出，halt_ret = 3**。

### 6.3 `--ebreak={halt, trap}` 开关

P3 的 ebreak 被设计成"测试退出钩子"——`temu_set_end(pc, a0)`。stage 5 的 48 个 isa 测试 + 6 个 program 测试**全部依赖**这个语义：末尾放一条 ebreak、用 `a0` 当退出码。

P6a 把 ebreak 按 spec 改成"BREAKPOINT 异常陷入"会**同时破坏**全部旧测试。两种路：

| 方案 | 结果 |
|--|--|
| A. 彻底换语义 | 48 个旧测试重写 |
| B. 引入 runtime 开关 | 旧测试不动，新测试按需启用 trap 模式 |

选 B。CLI 新增 `--ebreak={halt, trap}`（`src/main.c:48`），默认 `halt` 保兼容：

```c
// src/isa/riscv32/inst.c:279
INSTPAT("0000000 00001 00000 000 00000 1110011", "ebreak", N, {
    if (g_ebreak_mode == EBREAK_HALT) {
        temu_set_end(s->pc, R(10));        /* 旧路径 */
    } else {
        trap_take(CAUSE_BREAKPOINT, s->pc, s->pc);   /* 新路径 */
    }
});
```

**⚠️ 不是"新旧两套测试"的原因**：`isa.c` 和 `prog.c` 都跑在同一个 temu binary 下。如果改成"ebreak 永远 trap、测试框架自己设 mtvec = 退出 handler"，那 48 个旧测试都要加 "先 CSRRW MTVEC" 前缀——4 条额外指令塞进每个测试，模板改写量大且测试可读性降低。`--ebreak` 开关是**一行 CLI 换 0 行测试改动**。

**取舍标签**：pragmatic choice。理论上 stage 6a 做完应该彻底走 spec，但 B 方案让"旧的还是旧的、新的按新的"，代价极低。

**验收矩阵**：

| 模式 | `tests/isa` | `tests/programs` | `test_trap` | `test_interrupt` |
|--|--|--|--|--|
| `halt`（默认）| ✅ | ✅ | ✅ | ✅ |
| `trap` | 48 个死循环 | 全部死循环 | ✅（不依赖 halt）| ✅ |

所以 `make test` **默认 halt**，CI 跑的是混合——isa.c 内部已经为 `test_trap` / `test_interrupt` 单独包了 `TEST_TRAP` 宏，里面 **主动设置 mtvec** 让 ecall 真的有 handler。

### 6.4 `mcause` 最高位区分中断 vs 异常

`mcause` 编码（Privileged Spec Table 3.6）：

```
Bit 31:     0 = 同步异常
            1 = 异步中断
Bits 30..0: 具体 cause code
```

TEMU 用到的：

```c
#define CAUSE_BREAKPOINT       3u                     /* bit 31 = 0 */
#define CAUSE_ECALL_M         11u                     /* bit 31 = 0 */
#define CAUSE_INT_MTI        (0x80000000u | 7u)       /* bit 31 = 1 */
```

Handler 第一件事通常是读 `mcause`，看 bit 31 分派：

```asm
csrrs t0, mcause, zero
bltz  t0, interrupt_handler    ; bit 31 = 1 → 负数（signed）
; 否则 exception_handler
```

这是一个**bit trick**——C 里 `(int32_t)mcause < 0` 等价于 "bit 31 set"，省一条 `srli + andi`。spec 故意把中断标志放 bit 31 就是为了这种编译优化。

---

## 7. Difftest × CSR

P4 的差分测试把 GPR + PC 对拍得很稳。P6a 引入 7 个 CSR，要把它们也加进比对。

### 7.1 两套 CSR 实现的对拍

主实现走**表驱动**（`csr.c`）。ref 实现 **故意**用 switch-case（`difftest.c:46`）：

```c
static word_t ref_csr_read(uint32_t addr) {
    switch (addr) {
    case CSR_MSTATUS:  return ref_csr.mstatus;
    case CSR_MIE:      return ref_csr.mie;
    /* ... */
    default:           return 0;
    }
}
```

为什么**故意不复用** `csr_read`？重申 P4 §9.3 的教训：**两套实现共享代码 = oracle 沉默**。如果主实现 `csr.c` 的 `find_by_addr` 有 bug（比如 `mepc` 和 `mcause` 的 entry 顺序写反，两者值互换），ref 走独立的 switch 就能**独立**告诉你"ref 的 mepc 和你不一样"。

### 7.2 `mip` 的豁免

`mip.MTIP` 由 `timer_mtime() >= mtimecmp()` 决定——**墙钟驱动**。两次 poll 差的时间点可能刚好一边 ≥ 一边 <，`MTIP` 两边值不同。

对策（`difftest.c:404`）：

```c
/* mip is intentionally excluded: its MTIP bit tracks hardware
 * wall-clock state that the two CPUs poll at slightly different
 * times. Snapshot instead — software writes to mip via csrrw are
 * ignored at difftest boundary. */
ref_csr.mip = csr.mip;
```

每步比对前，把 ref 的 `mip` **强行对齐** main 的 `mip`。这意味着 `mip` **完全不参与差分保护**——如果有人写错了 mip 的 bit 布局，difftest 不会抓到。

**代价是可控的**：`mip` 目前只有 MTIP 一个位，靠 `tests/isa/isa.c::test_interrupt` 的端到端行为（中断真的触发了 3 次）补偿。

### 7.3 中断发生时 ref 的同步

定时器中断触发（`cpu_exec.c:115`）：

```c
trap_take(CAUSE_INT_MTI, 0, cpu.pc);
trap_commit();
paddr_touched_mmio = true;   /* let difftest resync ref */
```

**关键是最后一行**。P4 §6.1 的 `paddr_touched_mmio` snapshot 机制：此 flag 为真，difftest 跳过比对、直接把 main 的状态 `memcpy` 给 ref。

为什么复用它？本质是同一个问题：**main 和 ref 在"外部世界事件"面前无法严格对拍**——MMIO 写是副作用外泄，定时器中断是墙钟不确定。两者都靠"快照 ref = main 放弃比对"解决。

**代价**：中断触发那一步 CSR 不参与比对。如果 `trap_commit` 有 bug（比如 `mstatus.MPIE` 位号写错），**第一次**触发时逃过对拍；但**第二次**中断 trap 会基于错的 MPIE 恢复 MIE，很快显化成"下一步没再触发中断" / "handler 嵌套"。

### 7.4 CSR 对拍的具体 6 个字段

```c
CSR_CMP(mstatus);
CSR_CMP(mie);
CSR_CMP(mtvec);
CSR_CMP(mscratch);
CSR_CMP(mepc);
CSR_CMP(mcause);
/* mip 被强制同步，不比对 */
```

6 个参与、1 个豁免。`CSR_CMP` 宏展开是 `if (csr.X != ref_csr.X) panic(...)`，失败立刻 abort 并 dump 两边值。

---

## 8. 完整时序：一次定时器中断

把所有零件连起来，看一次中断**从起到落**。场景：

- 指令 `addi t0, t0, 1` 在 `pc = 0x80000020`
- `mtvec = 0x80000100`
- `mstatus.MIE = 1`、`mie.MTIE = 1`
- `mtimecmp = 0`，所以 `timer_mtime()` 必然 ≥ mtimecmp

```
cycle N: exec_once 开始
  ┌─────────────────────────────────────────────────┐
  │ 1. Fetch: inst @ 0x80000020                     │
  │ 2. Decode: s.pc = 0x20, s.snpc = 0x24           │
  │ 3. isa_exec_once: addi 执行 → gpr[t0]++         │
  │ 4. cpu.pc = s.dnpc = 0x24                       │
  │ 5. gpr[0] = 0                                   │
  │ 6. trap_pending() == false (无同步异常)          │
  │ 7. maybe_take_interrupt:                        │
  │      mstatus.MIE = 1    ✓                       │
  │      timer_mtime() >= mtimecmp → mip.MTIP = 1   │
  │      mie.MTIE & mip.MTIP = 1  ✓                 │
  │      trap_take(MTI, 0, cpu.pc=0x24):            │
  │          pending.epc = 0x24                     │
  │      trap_commit():                             │
  │          csr.mepc    = 0x24                     │
  │          csr.mcause  = 0x80000007               │
  │          mstatus: MPIE=1, MIE=0, MPP=M          │
  │          cpu.pc = mtvec & ~3 = 0x80000100       │
  │      paddr_touched_mmio = true                  │
  │ 8. difftest_step: 见 flag, snapshot ref         │
  └─────────────────────────────────────────────────┘

cycle N+1: exec_once @ 0x80000100 (handler 第 1 条)
  handler 做业务（计数、重设 mtimecmp ...）
  ...
  最后执行 mret:
    trap_mret(&s.dnpc):
        s.dnpc = csr.mepc = 0x24
        mstatus: MIE = MPIE = 1, MPIE = 1
    cpu_exec 末尾: cpu.pc = s.dnpc = 0x24

cycle N+K: exec_once @ 0x80000024
  取指 @ 0x24 = 被打断指令的下一条
  程序感觉不到被中断过 (除了 t0 可能被 handler 改)
```

**图解的要点**：

1. **中断发生**在指令边界——不是指令中间
2. **mepc 存的是 0x24**（下一条），不是 0x20（被打断的）
3. **mstatus.MIE 被清零**后，handler 中不会再中断（除非手动重开）
4. **mret 的返回 PC 通过 s.dnpc**（保持 INSTPAT 纯净），最后由 cpu_exec 末尾落到 cpu.pc
5. **difftest snapshot**——避免 ref 独立触发中断造成分歧

---

## 9. 理论视角

### 9.1 Privilege Levels：M / S / U 的演化

RISC-V 特权级分 4 档：

| 等级 | 符号 | 典型角色 |
|--|--|--|
| Machine | M | 固件、bootloader、hypervisor monitor |
| Hypervisor | H（v1.12 扩展）| 虚拟化 |
| Supervisor | S | OS kernel |
| User | U | 应用程序 |

Stage 6a 只有 **M-mode**——所有代码都是"最高权限"，`mstatus.MPP` 永远 M，没有真正的权限隔离。这是教学简化。

**x86 / ARM 的类比**：

- x86 有 Ring 0/1/2/3（主流 OS 只用 Ring 0 和 Ring 3，中间被忽略）
- ARM 有 EL0/1/2/3

**核心差别**：RISC-V 的特权切换是 **trap 驱动**——用户态用 `ecall` 升级、`mret` / `sret` 降级。没有"主动切换特权级"的指令（因为那就等于"用户可以提权自己"）。

**为什么有这么多层**：

- **M-mode** 管硬件（直接操作物理地址、所有 CSR 可读写）
- **S-mode** 管页表、进程切换，但**不能**直接访问物理地址的全部
- **U-mode** 只能看自己的虚拟地址空间，任何 CSR 访问都 trap

**这一层层"越权就陷入"的机制**是 OS 隔离的根基。没有特权级，用户进程可以直接改页表、关中断、崩掉整个系统。

### 9.2 上下文保存：硬件和软件的分工线

中断发生时，"上下文"是什么？严格说是**让 handler 结束后能无损返回的全部状态**。包括：

- **32 个 GPR**（128 字节）
- **PC**
- **mstatus 的 MIE/MPP 等位**
- **页表基址寄存器**（S-mode）
- **浮点寄存器**（F 扩展）
- ……

**问题**：这些谁负责存？硬件还是软件？

**RISC-V 的分工线**（极简主义）：

| 状态 | 硬件存 | 软件存 |
|--|--|--|
| PC | ✅ `mepc` | — |
| mstatus 的 MIE / MPIE / MPP | ✅ 自动位移 | — |
| mcause、mtval | ✅ | — |
| 32 个 GPR | ❌ | handler 开头 `sw x1,-4(sp); sw x2,-8(sp); ...` |
| 浮点寄存器 | ❌ | handler 按需 save |
| 页表基址（satp）| ❌ | 如果 handler 切换地址空间才需要 |

**硬件只存** PC 和 mstatus 栈帧——**因为那是软件无法**在 trap 瞬间**存下的**（保存 GPR 要用 GPR，自己砸自己的脚）。

**x86 的对比**：x86 硬件会自动存一个大 **interrupt frame**（EFLAGS, CS:EIP, SS:ESP, ...）——好处是软件 handler 可以直接 `iret` 回去；坏处是固定开销大、难以精简。

**RISC-V 的极简哲学**：

- 硬件只做**软件自己做不了**的部分
- 剩下的给软件自由安排

这让 RISC-V 中断进入**极快**（只动 2-3 个 CSR），代价是 handler 必须小心手动保存 GPR。xv6 的 `uservec` 代码就是在做这件事——开头 10 多行 `sd x1, 40(a0); sd x3, 56(a0); ...` 把 GPR 全存到 "trap frame" 内存里。

### 9.3 中断控制器的演化：PIC → APIC → PLIC

本章 TEMU 的中断设计**极简**：只有一个定时器中断，`mie.MTIE` 单 bit 使能，`mip.MTIP` 单 bit pending。真机远比这复杂。

**1981 – Intel 8259 PIC**（Programmable Interrupt Controller）：

IBM PC 的中断控制器。一块独立芯片，管 8 根中断线（IRQ0-IRQ7）。CPU 的 INT 引脚连 8259 的输出——多个设备中断 OR 到一起。8259 用**优先级寄存器**决定哪个先送给 CPU。

问题：8 根线不够。PC/AT (1984) 级联两个 8259 凑出 15 根（IRQ0-IRQ15，IRQ2 当级联口）。

**1990s – APIC**（Advanced PIC）：

Pentium 引入，分成：

- **Local APIC**：每 CPU 核一个，管该核的定时器、IPI（核间中断）
- **I/O APIC**：系统全局，24 路中断输入

支持 SMP（多核），通过 MSI（Message Signaled Interrupt）让 PCIe 设备直接"写消息"到 APIC 触发中断。

**RISC-V – PLIC**（Platform-Level Interrupt Controller）：

和 CLINT（Core-Local Interruptor，管 timer + 软件中断）分工：

- **CLINT**：每核的本地中断（mtime、mtimecmp、msip）
- **PLIC**：外设中断路由（UART、网卡、磁盘 ...）

TEMU stage 6a 只实现了 CLINT 的**一小部分**（mtime + mtimecmp），没做 PLIC。真要跑 xv6 / Linux 需要补上 PLIC 才能处理 UART 输入。

**教学留白**：6a 的定时器走 `mip.MTIP` **直连** CPU，不经过控制器。这是对的——定时器是 core-local 的，不需要全局路由。**非 core-local 中断**才需要 PLIC。

### 9.4 Nested traps 与 double fault

上面讲过 handler 可以手动重开 MIE 让自己被打断。但如果打断发生在 handler **刚进门**（还没存完 GPR）？

**递归 trap 的危险**：

```
main cpu.pc = 0x100
    → ecall (trap)
    → handler cpu.pc = 0x1000
        → 新中断 (如果 MIE 又开了)
        → handler v2 cpu.pc = 0x1000 (同一个 mtvec)
        → mepc 被覆盖成 v1 handler 的 PC！
        → 原始的 main PC 丢了
```

**Spec 的保护**：trap 发生时硬件自动清 MIE，所以**默认不会嵌套**。只有 handler 主动重开才会。

**Double fault**：handler 本身触发异常（比如 handler 代码段缺页）。RISC-V 里这就是再次走 trap 流程——如果 `mtvec` 指向的代码**再次**出问题，会死在里面。真 x86 会把这种情况分类成 **double fault**，再次失败变 **triple fault**（CPU 重置）。

**TEMU 的 assert**：`trap_take` 开头 `Assert(!pending.active, ...)`——检测"一条指令触发两次 trap"。这是 stage 6a 的不变量（单线程 + 顺序执行），stage 6b 引入 U-mode 后可能要放宽。

### 9.5 Polling vs Interrupt：不是"哪个更好"

本章开头用"轮询 vs 中断"做对比。但工程上两者**都用、都必要**。

**什么时候用轮询**：

- 高吞吐数据通道（10GbE 网卡、NVMe 磁盘）——中断开销大于数据处理开销时，不如不中断
- 实时系统——中断延迟抖动大，轮询延迟稳定
- 短时等待（自旋锁、I/O port 忙等 ≤ μs）

**什么时候用中断**：

- 稀疏事件（键盘按键，每秒 0-10 次）
- 长时等待（磁盘 I/O 100ms 级）
- 需要 CPU 睡眠省电

**现代 Linux 的混合**：`NAPI`（New API）网卡驱动 —— 收到一个中断后切到轮询模式，吃完 ring buffer 再开中断。两者最优组合。

> **核心洞察**：**轮询和中断不是互斥选项，是状态机的两个节点**。合适的系统**根据负载切换**：低负载中断省电，高负载轮询省开销。这是 data-path 设计的核心权衡。

---

## 10. 踩坑清单

### 10.1 `mepc` 同步异常忘加 4

Handler 收到 ecall，不 `mepc += 4` 直接 `mret` → 死循环回 ecall 本身。**最常见的 P6a bug**。规则记忆法：**同步异常 = 当前 PC，软件负责跳过**；**异步中断 = 下一条，软件直接 mret**。

### 10.2 `mstatus.MPIE` / `MIE` 位号写反

`MIE = bit 3`、`MPIE = bit 7`。写反了 trap 后 handler 看 MIE=1 自我递归、栈爆。单元测试 `"mret restores MIE"` 的 halt_ret = 8（= bit 3）不是 128——这个常量是**位号检查仪**，错了立刻炸。

### 10.3 `mtvec` 漏 `& ~3u`

Mtvec 低 2 位是模式位。直接 `cpu.pc = csr.mtvec` → 如果软件写入 `0x80000001`（低 2 位 = `01` = vectored 模式），CPU 跳到奇地址。**必须 mask 低 2 位**。

### 10.4 `csrrs rd, csr, x0` 被误认为"写入"

Spec 规定 `rs1 = x0` 的 `csrrs` / `csrrc` **不写 CSR**。实现时要看**指令字里的 rs1 编码位**，不是看 `src1` 的运行时值。`BITS(inst, 19, 15) != 0` 是正确的 gate。

### 10.5 在 INSTPAT body 里直接改 `cpu.pc`

P3 的铁律：INSTPAT 只通过 `s->dnpc` 影响 PC。`ecall`/`ebreak-trap` 想改 PC 必须走 `trap_take` staging；`mret` 写 `*dnpc`。**都不**在 body 里 `cpu.pc = ...`。

### 10.6 中断 gating 忘 MIE 或忘 MIP

三层门 `MIE & mie.MTIE & mip.MTIP`，漏任一层 → 要么中断永远不触发，要么中断失控刷屏。正确位置：cpu_exec 末尾先更新 mip.MTIP，再 AND 三层。

### 10.7 `mcause` bit 31 搞反

中断 = bit 31 = 1（负数），异常 = bit 31 = 0。Handler 分派用 `bltz mcause, interrupt_path`——想"简化"成 `beqz mcause, exception` 会漏掉 `mcause = 0`（INST_ADDR_MISALIGNED）这种合法异常。

### 10.8 `paddr_touched_mmio` 在中断触发时忘置位

`trap_commit` 之后必须 `paddr_touched_mmio = true`，让 difftest 跳过这步比对。忘了 → main 跳 mtvec、ref 还在原地，下一步立刻报 PC 分歧。

### 10.9 共享 `ref_csr_read` / `csr_read` 代码

Difftest 的价值建立在两实现**独立**。复用代码 = oracle 沉默——CSR 号映射错两边同错、对拍静默通过。手写两份即使重复也值得。

### 10.10 测试 `--ebreak=trap` 时忘了设 mtvec

开 `--ebreak=trap` 后所有 ebreak 都走 trap。如果 mtvec = 0（默认），trap 会跳到 `0x00000000` → paddr_read panic。`test_trap` 里必须在第一条 ebreak 之前 CSRRW 一个合法 mtvec。

### 10.11 `mscratch` 没用起来就白给

`mscratch` 设计初衷是 handler 开头"借一个寄存器存 sp"：`csrrw sp, mscratch, sp` 原子交换——handler 开头拿到 kernel stack，结束前还原。TEMU stage 6a 没跑复杂 handler 用不上它，但代码里已预留。Stage 6b 的 xv6 移植立刻会用到。

### 10.12 `--ebreak` 默认值改了破坏所有旧测试

默认**必须**是 halt。如果哪天鬼使神差把默认改成 trap，`make test` 会突然全红——而表面原因看起来像"ISA 实现错了"。防御：CLI 解析里对默认值加注释、改动时改 `main.c` 和 README 同时改。

---

## 11. 动手练习

### Easy 1 · 添加 `mhartid` 只读 CSR

`mhartid` (0xF14) 返回当前 hart（hardware thread）号，单核系统永远是 0。加一行 entry，`csr_write` 忽略对它的写。

**学到**：只读 CSR 的表达方式——表驱动下一个 "read handler / write handler" 分离的扩展口。

### Easy 2 · `info c` 按特权级分组

目前 `info c` 按注册顺序打印。改成按 addr 分组：`0x3xx` (M-mode) / `0x7xx` (debug) / 未来 `0x1xx` (S-mode)。在每组前打印一行 header。

**学到**：表驱动的排序技巧——改输出不改语义。

### Medium 1 · 实现 `wfi`（wait-for-interrupt）

指令编码 `0001000 00101 00000 000 00000 1110011`。语义：如果没有 pending 中断，**暂停取指**直到有。TEMU 里最简实现：`while (!(csr.mip & csr.mie) & MSTATUS_MIE) { /* 推进墙钟 */ sleep_us(1); }`。

**学到**：**省电指令**——真硬件上 wfi 停时钟，TEMU 只能模拟"啥也不干"。OS 内核的 idle loop 靠它省电。

### Medium 2 · `--itrace-trap`：trap 发生时打印行

现有 itrace 只记录普通指令。扩展：trap_commit 时记录一条"TRAP cause=... mepc=... mtvec=..."；mret 时记录"MRET to ..."。对调试 handler 极有用。

**学到**：itrace 的条目类型扩展；在 cpu_exec 的哪个点挂钩不破坏现有流。

### Medium 3 · `--no-difftest-csr` 隔离 CSR 对拍问题

临时开关：对拍时把所有 CSR 当 `mip` 处理（同步 ref := main，不比对）。排查"难重现的 CSR 假阳"时有用。

**学到**：诊断开关的"有选择地关闭 oracle" 模式——对拍失败时二分定位。

### Hard 1 · 实现 vectored mtvec

`mtvec[1:0] = 01` 时，异常跳 base，中断 i 跳 `base + 4*i`。改 `trap_commit` 读 mtvec 模式位。写一个 demo：异常和定时器中断各跳不同 handler。

**学到**：trap 向量化；handler 减少一次 cause 分派。

### Hard 2 · 接入 Spike 做权威 CSR 对拍

用 Spike 作为第三方 oracle（P4 §9.3 作者共享型风险的缓解）。挂接口：每条指令从 TEMU 和 Spike 各取 GPR/CSR/PC 比对。**只在 CSR 改动**时触发——减少 Spike 调用开销。

**学到**：工业级 oracle 接入；"按需调用" 的优化模式。

### Hard 3 · 实现 `medeleg` / `mideleg`（stage 6b 预习）

引入 delegation：M-mode 可以把特定异常/中断"下放"给 S-mode 处理，不再 trap 到 mtvec。现在还没 S-mode 可以做一半——medeleg/mideleg 写入并读出，trap 路径里加检查、发现 delegated 就"假装走 S 路径"（stvec 现在没实现，先 `panic("delegated but no S-mode")`）。

**学到**：为 6b 做骨架，理解"硬件帮 OS 做事"的通用模式。

---

## 12. 本章小结

**你应该能做到**：

- [ ] 解释 CSR 为什么独立于 GPR（权限、数量、副作用三理由）
- [ ] 写出 Zicsr 6 条的 INSTPAT 骨架
- [ ] 说清 `trap_take` / `trap_commit` / `trap_mret` 分别做什么
- [ ] 画出一次定时器中断从触发到返回的完整时序
- [ ] 解释 `mepc` 在同步 vs 异步 trap 下的存储规则差异
- [ ] 说出 `--ebreak={halt, trap}` 为什么要共存

**你应该能解释**：

- [ ] staging + commit 分两步实现而非一把改 `cpu.pc` 的原因
- [ ] `mstatus.MIE` / `MPIE` 的"深度 1 栈"语义和它限制了 handler 什么
- [ ] 硬件 vs 软件在"上下文保存"上的分工线（RISC-V 极简 vs x86 大 frame）
- [ ] `mip` 在 difftest 里必须豁免的原因（墙钟不可重现）
- [ ] 中断控制器从 PIC → APIC → PLIC 的演化动机
- [ ] 为什么轮询和中断不是互斥选项

---

## 13. 延伸阅读

- **RISC-V Privileged Spec v1.12, 第 3 章（Machine-Level ISA）** —— `mstatus / mtvec / mepc / mcause` 的权威定义，本章术语全来自此处
- **RISC-V Privileged Spec, Table 3.6** —— 所有 cause 编号；stage 6b 做缺页时再来查
- **xv6-riscv `kernel/trap.c` + `kernel/trampoline.S`** —— 真实 OS 的 handler：GPR 保存、特权级切换、mret 回用户态。读完你就懂 stage 6d 要做什么
- **"A 10-Page Introduction to the RISC-V Privileged Architecture" (Waterman, 2019)** —— 官方作者的 tutorial，比 spec 好读
- **Linux Kernel Documentation `Documentation/translations/zh_CN/IRQ.txt`** —— 中文版 Linux 中断子系统概览
- **Intel® 64 IA-32 Software Developer Manual Vol. 3A, 第 6 章（Interrupt and Exception Handling）** —— x86 的对比——读完你会发现 RISC-V 极简到什么程度
- **SiFive FU540-C000 Manual** —— 真芯片的 CLINT + PLIC 实现参考

---

## 与后续章节的连接

| 下一章做什么 | 本章埋下的伏笔 |
|--|--|
| P6b U-mode + ecall 做系统调用 | `mstatus.MPP` 已经写对，切 U 只改一位；`ecall` cause 号已经分好 U/M |
| P6c 虚拟内存（satp + 页表）| CSR 表驱动扩展：加一行 `satp` entry；缺页走同一 trap 路径 |
| P6d 移植 xv6 | mtvec 挂 xv6 的 `uservec`；`mscratch` 终于有用武之地 |
| 任何加 cause 码的 stage | `trap.h` 的 CAUSE 宏加常量，`mcause` 比对自动覆盖 |

**取舍标签**：theoretical ideal。Stage 6a 是 TEMU 第一次**按 spec 字面量**实现一套机制——mstatus 的 MPIE/MIE 栈、mtvec 的模式位、mcause 的 bit 31 都是 spec 的原文。

做完你会发现一个有趣的心得：**"中断"这个在计组课上最抽象的概念**，落到 C 代码其实就是 **trap_commit 那几行 `csr.mepc = ...`**。"保存现场"不是一句口号，是 `csr.mstatus |= (mie << MPIE_BIT)` 那一个位。

**下一站：P6b——U-mode 特权级与系统调用，让 TEMU 第一次学会区分"用户"和"内核"。**
