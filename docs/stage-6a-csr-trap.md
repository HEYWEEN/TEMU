# Stage 6a — CSR 寄存器 + Trap 机制 + 定时器中断

> **状态**：📝 设计稿（stage 6 的第一个子阶段）
> **前置**：stage 0–5 全绿
> **验收命令**：`make test-stage6a` — 一个裸机小程序能被定时器中断打断、跳进 handler、`mret` 回来继续执行

---

## 1. Problem motivation

到 stage 5 我们的 CPU 只会 **顺序执行**：取指 → 执行 → PC += 4。它没有办法响应"外部事件"——比如定时器到点了、串口来字符了。

**操作系统的前提是这件事**：OS 能从用户程序手里"夺回"控制权，靠的就是**陷入（trap）**。陷入不是一条指令，是一套机制：

- CPU 在执行任意指令时，被迫停下
- 把当前 PC 存起来（`mepc`）
- 跳到一个预先设定的处理地址（`mtvec`）
- 处理完后 `mret` 回到原来的 PC 继续

没有这套机制，后面的用户态隔离、系统调用、抢占式调度**全都做不了**。

---

## 2. Naive solution & why not

> 直接在 CPU 主循环里硬编码 "每 10000 条指令调用一次 `handler()`"。

这能让定时器粗暴地"打断"程序——但：

- **绕过了 ISA 规范**：真硬件没有这种语义，程序跑在 Spike 或真芯片上行为不一样
- **没法保存上下文**：handler 返回后怎么让用户程序感觉不到被打断过？
- **没有层次**：异常（除零、非法指令）、系统调用、外部中断需要**同一套**机制统一处理

所以必须按 RISC-V Privileged Spec 实现 M-mode CSR + `mtvec` + `mret`。

---

## 3. Key ideas

### 3.1 CSR：控制状态寄存器

独立于 32 个 GPR 的**另一组寄存器**，通过 `csrrw/csrrs/csrrc` 指令访问。每个 CSR 有 12 位编号（0x000–0xFFF），不是线性数组——用**稀疏表**或 switch-case 映射。

Stage 6a 只实现 7 个 M-mode CSR：

| 编号 | 名字 | 作用 |
|------|------|------|
| 0x300 | mstatus | 状态位（MIE、MPIE、MPP）|
| 0x304 | mie | 中断使能屏蔽 |
| 0x305 | mtvec | trap handler 入口地址 |
| 0x340 | mscratch | handler 暂存寄存器 |
| 0x341 | mepc | trap 时的 PC 快照 |
| 0x342 | mcause | trap 原因 |
| 0x344 | mip | 中断 pending 位（只读视角；在模拟器里由 cpu_exec 写）|

其他 CSR（`mhartid / medeleg / mideleg / mtval` 等）**读零、写忽略**——先跑通再扩。

### 3.2 Trap 机制骨架

```c
/* 同步陷入：ecall / illegal instruction / (stage 6b) misaligned */
void trap(uint32_t cause, uint32_t tval) {
    csr.mepc    = cpu.pc;          /* 被打断指令的 PC */
    csr.mcause  = cause;
    /* mstatus.MPIE <- MIE; MIE <- 0; MPP <- current_priv */
    csr.mstatus = update_mstatus_on_trap(csr.mstatus);
    cpu.pc = csr.mtvec & ~0x3u;    /* direct mode only */
}

void mret(void) {
    cpu.pc = csr.mepc;
    /* MIE <- MPIE; MPIE <- 1; priv <- MPP */
    csr.mstatus = update_mstatus_on_mret(csr.mstatus);
}
```

> **注意**：stage 6a 还没有 U-mode，MPP 一直是 M-mode，特权切换部分写但不生效（为 6b 做准备）。

### 3.3 定时器中断的闭环

```
硬件侧：
  mtime += 1 每条指令（或按真实时间）
  当 mtime >= mtimecmp 时，mip.MTIP = 1

CPU 主循环每步末尾：
  if (mstatus.MIE && mie.MTIE && mip.MTIP) {
      trap(cause=TIMER_INT, tval=0)
  }
```

`mtimecmp` 是新的 MMIO 寄存器（64-bit，地址 `0xa0000040–48`），软件通过 `sw` 写入触发阈值。

### 3.4 `ebreak` 的兼容开关

现在 `ebreak` 被占用作测试退出钩子（`temu_set_end`）。stage 6a 引入 `--ebreak=halt|trap` 命令行开关：

- `halt`（默认）：继续是测试退出
- `trap`：按规范走 trap，`mcause = 3`（breakpoint）

所有现有测试（48 isa + 6 program）用 halt 模式不动；stage 6a-3 的新 trap 测试用 trap 模式。

---

## 4. Mechanism：四个 chunk 拆解

### 4.1 Chunk 6a-1：CSR 寄存器文件 + `info c`

**新文件**：`src/isa/riscv32/csr.c` + `src/isa/riscv32/local-include/csr.h`

```c
/* csr.h */
typedef struct {
    word_t mstatus;
    word_t mie;
    word_t mtvec;
    word_t mscratch;
    word_t mepc;
    word_t mcause;
    word_t mip;
} CSR_state;

extern CSR_state csr;

/* 通用读写接口。未实现的 CSR 号：read 返回 0，write 静默丢弃。
 * 非法 CSR 号（stage 6b 会区分）：暂时按"未实现"处理。 */
word_t csr_read (uint32_t addr);
void   csr_write(uint32_t addr, word_t val);

const char *csr_name(uint32_t addr);   /* 用于 info c 打印 */

void csr_init(void);
```

**改动**：
- `include/cpu.h`：不动（CSR 不塞进 `CPU_state`，保持 struct 纯净）
- `src/isa/riscv32/reg.c` 的 `cpu_init` 里调用 `csr_init()`
- `src/monitor/sdb.c` 的 `info` 命令分派加 `info c`
- `isa_reg_val`（expr 求值器用）扩展：识别 `mstatus / mepc / ...` 7 个名字

**验收**：
```
(temu) info c
  mstatus  : 0x00000000
  mie      : 0x00000000
  mtvec    : 0x00000000
  mscratch : 0x00000000
  mepc     : 0x00000000
  mcause   : 0x00000000
  mip      : 0x00000000
(temu) p $mtvec
$1 = 0
```

### 4.2 Chunk 6a-2：CSR 指令 + difftest 扩展

**改动 `src/isa/riscv32/inst.c`**：加 6 条 INSTPAT

```c
/* --- CSR (Zicsr) ---------------------------------------- */
INSTPAT("??????? ????? ????? 001 ????? 1110011", "csrrw", I, { ... });
INSTPAT("??????? ????? ????? 010 ????? 1110011", "csrrs", I, { ... });
INSTPAT("??????? ????? ????? 011 ????? 1110011", "csrrc", I, { ... });
INSTPAT("??????? ????? ????? 101 ????? 1110011", "csrrwi", I, { ... });
INSTPAT("??????? ????? ????? 110 ????? 1110011", "csrrsi", I, { ... });
INSTPAT("??????? ????? ????? 111 ????? 1110011", "csrrci", I, { ... });
```

**语义要点**：
- `csr_addr = inst[31:20]`（不是立即数，是 12 位编号）
- `rs1 = 0` 时 `csrrs/csrrc` **不**写 CSR（规范要求，用于纯读）
- 原值先保存再写（防 `csrrw x1, mscratch, x1` 把 x1 覆盖）
- `csrrwi/csrrsi/csrrci` 的"立即数"是 `rs1` 字段的 5 位无符号值

**difftest 扩展**：`src/difftest/difftest.c`
- 加 `static CSR_state ref_csr;`
- 第二实现的 `case 0x73:` 分支把 `f3 != 0` 的情况走 CSR 读写（独立手写，刻意不复用主实现）
- `difftest_step` 比对集合加 7 个 CSR

> ⚠️ **诚实说明**：我们的 difftest 是**自写的第二实现**，不是 Spike。两实现可能同时写错 CSR 语义——这时 difftest 保护不住。针对 CSR 语义，我会额外在 `tests/isa/csr/` 里放 5–6 个**手算预期值**的黄金用例，跟 difftest 互补。

**验收**：
- `tests/isa/csr/` 新增用例全绿
- 跑 `csrrw t0, mtvec, t1` 后 `info c` 看到 mtvec 已更新

### 4.3 Chunk 6a-3：trap + mret + ebreak 开关

**新文件**：`src/isa/riscv32/trap.c`

```c
/* cause 编码常量（见 Privileged Spec Table 3.6）*/
#define CAUSE_INST_ADDR_MISALIGNED  0
#define CAUSE_ILLEGAL_INST          2
#define CAUSE_BREAKPOINT            3
#define CAUSE_ECALL_U               8
#define CAUSE_ECALL_M              11
#define CAUSE_INT_MTI      (0x80000000u | 7)   /* machine timer int */

void trap_take (uint32_t cause, uint32_t tval);   /* 同步异常 */
void trap_mret (void);
```

**改动 `inst.c`**：
```c
INSTPAT("0000000 00000 00000 000 00000 1110011", "ecall", N,
        trap_take(CAUSE_ECALL_M, 0));
INSTPAT("0000000 00001 00000 000 00000 1110011", "ebreak", N, {
    if (config.ebreak_mode == EBREAK_HALT)
        temu_set_end(s->pc, R(10));
    else
        trap_take(CAUSE_BREAKPOINT, s->pc);
});
INSTPAT("0011000 00010 00000 000 00000 1110011", "mret", N,
        trap_mret());
```

**改动 `main.c`**：解析 `--ebreak=halt|trap` 参数，默认 halt。

**难点：`trap_take` 要改 `s->dnpc`，不能直接改 `cpu.pc`**  
因为 stage 3 的约定是 INSTPAT 只写 `s->dnpc`，cpu_exec 末尾才把 dnpc 落到 cpu.pc。所以 `trap_take` 的签名其实要拿到 Decode 指针。两种做法：

| 做法 | 利 | 弊 |
|------|-----|-----|
| A. 加全局 `pending_trap` 标志，cpu_exec 末尾检查 | 保持 INSTPAT 纯粹 | 多一层间接 |
| B. `trap_take(Decode *s, ...)`，INSTPAT 里传 `s` | 直接 | INSTPAT 要改签名，但只有 3 条用到 |

**选 A**。定时器中断（6a-4）本来就要在 cpu_exec 末尾处理，同一个机制两用。

**新增测试 `tests/programs/trap-ecall.c`**：
```c
// 用 ebreak=trap 模式
// 预置 mtvec 指向 handler；handler 把 a0 写成 42 然后 mret
// main ecall → trap → handler → mret → 验证 a0==42
```

**验收**：
- `make test-prog PROG=trap-ecall EBREAK=trap` 通过
- `make test` 全旧测试仍全绿（halt 模式）

### 4.4 Chunk 6a-4：定时器中断 + demo

**改动 `src/device/timer.c`**：
- 保留现有 `mtime` 低/高 32 位（offset 0/4）
- 新增 `mtimecmp` 64-bit 可读写寄存器（offset 8/12）
- 写 `mtimecmp` 不触发副作用；每条指令末尾由 cpu_exec 轮询

**改动 `src/cpu/cpu_exec.c`** 的 `exec_once` 末尾：

```c
static void maybe_take_interrupt(void) {
    if (!(csr.mstatus & MSTATUS_MIE)) return;

    /* 更新 mip.MTIP */
    if (timer_mtime() >= timer_mtimecmp()) {
        csr.mip |= MIP_MTIP;
    } else {
        csr.mip &= ~MIP_MTIP;
    }

    if ((csr.mip & csr.mie) & MIP_MTIP) {
        trap_take(CAUSE_INT_MTI, 0);
    }
}
```

调用位置：`isa_exec_once` 之后、`difftest_step` **之前**——因为 difftest 的第二实现也要模拟同一次中断，两侧状态才对得上。

**difftest 的处理**：中断到来那一瞬间，两实现都跳 mtvec。但**定时器值是墙钟**——两次读可能不同。处理：定时器 MMIO 访问已经有 `paddr_touched_mmio` 机制跳过当轮比对，中断触发把 ref_cpu.pc 也同步过去即可（借用同一机制）。

**demo `tests/programs/trap-timer.c`**：
```c
volatile int counter = 0;

void __attribute__((aligned(4))) handler(void) {
    counter++;
    // 重置 mtimecmp += 1000us
    uint64_t now = read_mtime();
    write_mtimecmp(now + 1000);
    asm volatile("mret");
}

int main(void) {
    set_mtvec((uint32_t)handler);
    write_mtimecmp(read_mtime() + 1000);
    csrrs_mstatus(MSTATUS_MIE);
    csrrs_mie(MIE_MTIE);
    while (counter < 3);     // 被打断 3 次后退出
    return counter;           // ebreak, halt mode, a0=3
}
```

**验收命令**：
```bash
make test-stage6a
# Expected: trap-timer.bin exits with 3
```

---

## 5. Concrete example — 一次定时器中断完整流程

```
假设 mtvec=0x80000100, mtimecmp=0x1000, 当前 pc=0x80000020
指令: addi t0, t0, 1
exec_once:
  1. isa_exec_once: 正常执行 addi，s->dnpc = 0x80000024
  2. cpu.pc = 0x80000024, gpr[0]=0
  3. maybe_take_interrupt:
     - mstatus.MIE=1, mie.MTIE=1
     - timer_mtime() = 0x1000 (到点了) → mip.MTIP=1
     - 命中！调用 trap_take(CAUSE_INT_MTI, 0):
         csr.mepc     = 0x80000024   (下一条指令，不是被打断的)
         csr.mcause   = 0x80000007
         csr.mstatus.MPIE = csr.mstatus.MIE (=1)
         csr.mstatus.MIE  = 0
         cpu.pc = 0x80000100 (mtvec)
  4. difftest_step: ref_cpu 同步跳到 mtvec

下一轮 exec_once:
  取指 @0x80000100 = handler 第一条
  ... handler 递增 counter、重置 mtimecmp ...
  mret:
    cpu.pc = csr.mepc (0x80000024)
    csr.mstatus.MIE = csr.mstatus.MPIE (=1)
    csr.mstatus.MPIE = 1
  → 回到被打断的下一条，继续
```

**关键细节**：`mepc` 存的是**下一条**指令的 PC（被中断发生时还没跳转），不是被打断的指令。RISC-V 规范：外部中断 `mepc = pc+4`；同步异常（ecall）`mepc = 当前 pc`，handler 自己 +4 跳过 ecall。实现时要区分。

---

## 6. Acceptance

| Chunk | 验收命令 | 预期 |
|-------|---------|------|
| 6a-1 | `make run` 后 `info c` | 打印 7 个 CSR = 0 |
| 6a-1 | `p $mtvec` | 求值成功 |
| 6a-2 | `make test-isa` | 48 旧 + 新 6 条 CSR 全绿 |
| 6a-2 | difftest 开启跑 `trap-timer.bin` 的 CSR 部分 | ref 和 main CSR 一致 |
| 6a-3 | `make test` | 旧测试全绿（halt 模式）|
| 6a-3 | `make test-prog PROG=trap-ecall EBREAK=trap` | 退出码=42 |
| 6a-4 | `make test-stage6a` | `trap-timer.bin` 退出码=3，耗时约 3ms |

---

## 7. Limitations / pitfalls

### 7.1 `mepc` 的"被打断 PC" vs "下一条 PC"

如上所述，**同步异常和外部中断规则不同**。最常见 bug：ecall 后 handler `mret` 回到 ecall 本身，死循环。Handler 必须自己 `mepc += 4`。

### 7.2 `mstatus.MIE` / `MPIE` 的栈式保存

`trap_take` 保存 MIE → MPIE 之后必须**清零 MIE**，否则 handler 执行时又被中断递归陷入，栈爆。`mret` 恢复时再取回。这是硬件自动做的，软件不用管，但模拟器实现里必须写对。

### 7.3 定时器值的精度

我们用墙钟 `gettimeofday`——**模拟器跑得慢时定时器相对"飞快"**。`trap-timer.c` 里把 mtimecmp 设成 `now + 1000us`，如果模拟器主循环一条指令就花了几十微秒，`while(counter<3)` 可能根本没进入就已经满足条件。**缓解**：mtimecmp 设大一点（10ms 级别），或 6a-4 时用"已执行指令数 / 假设 10MHz"当虚拟时钟。

### 7.4 difftest 的 CSR 盲区

两实现都我们自己写的，CSR 语义写反（比如 MPIE 和 MIE 位号搞错）两边都会错。**对策**：
- `tests/isa/csr/` 手工黄金用例（手算 mstatus 预期值）
- 对照 xv6-riscv 的 `kernel/riscv.h` 常量定义
- 遇到可疑 bug，起一个 Spike（`brew install spike`）临时做一次权威比对

### 7.5 INSTPAT 签名 vs trap 的 "dnpc"

见 4.3 决策 A：用全局 `pending_trap` 标志，INSTPAT 不改签名。要注意**同一条 ecall 在 isa_exec_once 里标记 pending_trap，cpu_exec 看到后用 `cpu.pc = mtvec`（而不是 dnpc）**。

---

## 8. 与后续 stage 的连接

| Stage 6a 留下的 | Stage 6b/c/d 会怎么用 |
|-----------------|----------------------|
| CSR 框架 | 加 sstatus/sepc/stvec/satp 只是扩表 |
| trap_take / mret | U-mode 陷入走同一入口，只改 MPP |
| mtvec 直接跳 | 6d 的 kernel trap handler 就挂在 mtvec |
| pending_trap 机制 | 缺页异常、非法指令都复用这条路径 |
| mtimecmp | OS 调度器的时钟片基础 |

---

## 9. 与 OS 课的连接

| 概念（用户笔记） | Stage 6a 对应实现 |
|-----------------|---------------------|
| 「中断 = 硬件主动改 PC」 | `maybe_take_interrupt` 里那几行 |
| 「系统调用是特殊的陷入」 | `ecall` → `trap_take(ECALL, 0)` |
| 「上下文保存」 | Handler 要软件 `sw x1, 0(sp)` 存 GPR；硬件只存 PC |
| 「中断控制器 PLIC」 | 6a 暂不实现，先用 mip/mie 直连 |

**取舍标签**：theoretical ideal。Stage 6a 是把"中断"这个计组课上最抽象的概念**落到每一行 C 代码**——做完你会发现"保存现场"不是一句口号，是 `csr.mstatus` 那一个位。
