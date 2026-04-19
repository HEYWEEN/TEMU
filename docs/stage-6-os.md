# Stage 6 — （可选）异常 / 分页 / 跑一个小 OS

> **状态**：📝 可选 stage，前置是 stage 0–5 全部稳定运行
> **验收命令**：能跑起一个用户态 hello world（内核 + 用户进程分地址空间）

---

## 1. Problem motivation

到 stage 5 我们有一台能跑裸机 RV32I 程序的模拟器。但**操作系统**的核心机制还没涉及：

- **用户态 / 特权态** 的切换（`ecall` / `mret`）
- **异常 / 中断** 的陷入 + 返回
- **虚拟内存**（分页 + MMU）
- 多个进程共用一套物理内存但看到不同地址空间

这些是操作系统课的核心，模拟器是检验学习的最好方式。

> **为什么标"可选"**：这个 stage 工作量 ≈ 前 5 个 stage 之和。做完你会有一台能跑自写 OS 的完整机器；不做它，stage 0-5 已经是一个**有价值的产物**了。

---

## 2. 目标拆解

如果做，按以下子阶段推进：

### 6a. M-mode 异常基础设施

- 实现 CSR（`mstatus / mepc / mcause / mtvec / mie / mip`）
- `ecall` 触发陷入：保存 PC 到 `mepc`，跳 `mtvec`
- `mret`：从 `mepc` 恢复 PC
- 定时器中断：TIMER MMIO 扩展，溢出时设 `mip.MTIP`

### 6b. 用户态 / 特权态

- 引入 `U-mode`
- `mstatus.MPP` 区分陷入前模式
- 非法指令 / 访存越权触发异常

### 6c. 分页（Sv32）

- 实现 `satp` CSR
- 实现 MMU：`vaddr → paddr` 通过两级页表翻译
- TLB（可选）：简单的 hash 表缓存翻译结果
- 缺页异常

### 6d. 跑一个最小 OS

可选参考：
- 自写一个 ~500 行的 kernel（调度器、ecall 分发、上下文切换）
- 移植 xv6-riscv 的 RV32 版本
- PA 讲义最后一阶段有 Nanos-lite（NJU 自家教学 OS）

---

## 3. Key ideas

### 3.1 CSR 指令

```
CSRRW rd, csr, rs1    # rd = csr, csr = rs1
CSRRS rd, csr, rs1    # rd = csr, csr |= rs1
CSRRC rd, csr, rs1    # rd = csr, csr &= ~rs1
（加 immediate 版 CSRRWI / CSRRSI / CSRRCI）
```

在 stage 3 的 INSTPAT 表里补这几条，值靠一张 CSR 编号 → 寄存器的映射表。

### 3.2 陷入机制骨架

```c
void trap(uint32_t cause, uint32_t tval) {
    csr_mepc    = cpu.pc;
    csr_mcause  = cause;
    csr_mtval   = tval;
    csr_mstatus = set_mpp(csr_mstatus, current_priv);
    current_priv = PRIV_M;
    cpu.pc = csr_mtvec;
}

void mret(void) {
    cpu.pc = csr_mepc;
    current_priv = mpp_of(csr_mstatus);
}
```

### 3.3 Sv32 分页

32 位虚拟地址被切成三段：

```
31      22 21      12 11           0
┌─────────┬──────────┬──────────────┐
│  VPN[1] │  VPN[0]  │    offset    │
└─────────┴──────────┴──────────────┘
```

`satp` 指向一级页表，一级页表项指向二级页表，二级页表项映射到 4KB 物理页。

**关键字段**：

```
PTE:
┌──────────┬──────┬───┬──────────────────────┐
│   PPN    │ RSW  │DAG│U X W R V             │
└──────────┴──────┴───┴──────────────────────┘
```

`V` valid、`R/W/X` 权限、`U` 用户态可访问、`A/D` access/dirty。

### 3.4 MMU hook 进 `vaddr_read/write`

```c
word_t vaddr_read(vaddr_t vaddr, int len) {
    paddr_t paddr = mmu_translate(vaddr, MEM_TYPE_READ);
    if (paddr == MMU_EXCEPTION) return 0;   // trap 已在内部触发
    return paddr_read(paddr, len);
}
```

取指也要走 MMU（除非还在 M-mode bare 模式）。

---

## 4. Concrete example —— 用户态 `write` syscall 走一遍

```
用户态 U-mode 程序:
  li a7, SYS_write
  la a1, msg
  li a2, 6
  ecall                          ← 触发 chunk #1

↓ trap ↓
mepc = pc_of_ecall
mcause = 8 (environment call from U-mode)
current_priv = M
pc = mtvec                        ← chunk #2: 跳到内核 trap handler

内核 trap handler:
  根据 mcause 派发
  case ECALL_U:
    switch (a7) {
      case SYS_write: sys_write(a0, a1, a2); break;
    }
  mepc += 4                       ← 跳过 ecall
  mret                            ← chunk #3

↓ mret ↓
current_priv = U
pc = mepc
继续用户态执行
```

整个流程涉及 **特权切换、CSR 保存恢复、MMU（如开启）**——每一步都必须和 RISC-V 规范完全一致。

---

## 5. Acceptance

| 子阶段 | 检查 |
|--------|------|
| 6a | 裸机定时器中断能触发，handler 返回后程序继续 |
| 6b | U-mode 执行 M-mode 指令触发 illegal instruction 异常 |
| 6c | 两段物理不重叠的页表，U-mode 看到的 `0x1000` 可以映射到不同物理页 |
| 6d | 两个用户进程分别输出各自字符串，不混杂 |

---

## 6. Limitations / pitfalls

### 6.1 CSR 规范巨大

RISC-V Privileged 手册 ~200 页，CSR 有几百个。**只实现需要的**：
- `mstatus / mepc / mcause / mtval / mtvec / mie / mip / mscratch / mhartid / mideleg / medeleg`（M-mode）
- `sstatus / sepc / scause / stvec / satp`（S-mode，若做）
- 其他读零写忽略

### 6.2 分页 bug 难调

页表项位 bit 5 写错整个系统直接"某个地址莫名访问失败"。**带 difftest 起步**——Spike 的 MMU 行为是对的，有它护航可以追到 PTE 级别的差异。

### 6.3 S-mode vs M-mode

真实 OS 基本跑 S-mode（有 satp、更轻量）。最小 OS 可以只用 M-mode 偷懒（满权限直接跑）。建议先 M-mode 版跑通 hello，再加 S-mode + satp。

### 6.4 上下文切换的寄存器保存

陷入时硬件只保存 PC（→ mepc）；**其他 31 个通用寄存器要由软件保存**。trap handler 的前几行一定是：

```asm
csrrw sp, mscratch, sp       # 切到内核栈
sw x1, 0(sp)
sw x5, 4(sp)
...                          # 保存所有需要的 GPR
```

忘了一个就是"莫名其妙的寄存器损坏"。

---

## 7. 资源

| 资源 | 用途 |
|------|------|
| RISC-V Privileged Spec | CSR 语义、trap 机制、MMU 格式 |
| xv6-riscv | 教学 OS 源码，~6000 行 C，写得极清晰 |
| PA4 讲义（Nanos-lite） | 如果走 NJU PA 路径，stage 6 约等于 PA3+PA4 |
| Spike 源码 | 当规范含糊时，看 Spike 怎么实现 |

---

## 8. 为什么值得做

这个 stage 把**操作系统课的全部理论**落地成能跑的代码。做完之后：

- 读 Linux 源码时你知道 trap.c 里在干什么
- 看到 `set_current_state(TASK_INTERRUPTIBLE)` 能想到对应的 `mstatus / mie` 操作
- 写驱动时理解 MMIO、volatile、内存屏障的**工程必要性**

> **取舍标签**：theoretical ideal。前 5 个 stage 是"工匠的手艺"，这个 stage 是"理论的验证"。
