# Stage 6b — U-mode 特权级 + 非法指令异常

> **状态**：📝 设计稿（stage 6 的第二个子阶段）
> **前置**：stage 6a 全绿（CSR / trap / 定时器中断跑通）
> **验收命令**：`make test-stage6b` — 一个带 "kernel + user" 结构的裸机镜像，user 程序通过 `ecall` 发起系统调用打印字符串，kernel 用 `mret` 让它回到 U-mode 继续跑；同一 user 程序执行 `mret` 会触发 illegal instruction 异常

---

## 1. Problem motivation

到 stage 6a 我们有了 trap 机制——但**所有代码还跑在 M-mode**。程序可以：

- 随意写任何 CSR（包括 `mtvec`，等于想改 handler 就改）
- 直接 `mret`
- 访问全部物理内存

这在 OS 里是不可接受的。**用户程序必须被关在一个权限盒子里**，只能通过受控入口（ecall）请求内核服务。这就是 **U-mode**：

| 能力 | M-mode | U-mode |
|------|--------|--------|
| 读写 M-mode CSR | ✅ | ❌ → illegal inst |
| 执行 `mret` | ✅ | ❌ → illegal inst |
| 执行 `ecall` | cause=11 | cause=8 |
| 访问物理内存 | 全部 | 看 PMP / 未来的 MMU |

> **Stage 6b 的边界**：只做特权态状态机 + 指令级权限检查。**不做**内存权限（那要 PMP 或 MMU，留给 6c）。所以 "U-mode 能随便访问物理内存" 这件事**保留不修**——它违反现实硬件，但对教学上的"ecall/mret 流程"没有影响。

---

## 2. Naive solution & why not

> "在 INSTPAT 里对每条敏感指令加 `if (current_priv != M) panic()`"

这能在模拟器层面拦住非法行为，但：

- **绕过了 ISA 规范**：真硬件不是 panic，是 trap 到 M-mode handler，handler 可以选择终止进程或模拟指令
- **没法测 kernel**：OS 的一个核心代码路径就是"处理 U-mode 的非法访问"，`panic` 直接废掉这条路径
- **堵不住 CSR**：CSR 读写是数据依赖的（地址在 rs1/rd 里），不能在 INSTPAT 那层静态判断

所以必须把 U-mode 做成一个**真实特权态**：有状态、有转换规则、违规走 trap 统一入口。

---

## 3. Key ideas

### 3.1 特权态是一个全局状态

加一个模块级变量 `current_priv`，三个取值：`PRIV_U=0 / PRIV_S=1 / PRIV_M=3`（S=1 是规范里的编号空洞预留，stage 6b 不用但编号要对）。

```c
/* src/isa/riscv32/priv.c */
static uint32_t current_priv = PRIV_M;   /* boot 时在 M-mode */

uint32_t priv_get(void) { return current_priv; }
void     priv_set(uint32_t p) { current_priv = p; }
```

**状态转换**只有三处：

| 事件 | 转换 |
|------|------|
| trap_commit | `MPP ← current_priv`；`current_priv ← M` |
| mret        | `current_priv ← MPP`；`MPP ← U` |
| reset       | `current_priv ← M` |

不是每条指令都要改——只在 trap 入口 / 返回的两个点。其他地方**只读**。

### 3.2 `mstatus.MPP` 真正开始工作

6a 里 MPP 一直写 M、读也是 M——等于摆设。6b 要让它真正记录前一特权态：

```c
/* trap_commit (6b 版本) */
word_t s = csr.mstatus;
...
s = (s & ~MSTATUS_MPP_MASK) | (current_priv << MSTATUS_MPP_LSB);
csr.mstatus = s;
current_priv = PRIV_M;

/* trap_mret (6b 版本) */
word_t mpp = (csr.mstatus >> MSTATUS_MPP_LSB) & 3u;
current_priv = mpp;
/* 规范：MPP ← U（后续 mret 栈倒空时默认最小权限） */
csr.mstatus = (csr.mstatus & ~MSTATUS_MPP_MASK) | (PRIV_U << MSTATUS_MPP_LSB);
```

### 3.3 `ecall` 按当前特权级派发

```c
INSTPAT("0000000 00000 00000 000 00000 1110011", "ecall", N, {
    word_t cause = (priv_get() == PRIV_U) ? CAUSE_ECALL_U : CAUSE_ECALL_M;
    trap_take(cause, 0, s->pc);
});
```

**注意 epc 传 `s->pc` 而不是 `s->dnpc`**：ecall 是同步异常，`mepc` 存"被打断的指令"，handler 自行 `mepc += 4`。6a 里已经这么写了，6b 不变。

### 3.4 非法指令异常：U-mode 碰 M-only 东西

三类违规：

1. **`mret` 在 U-mode**：整个 trap 返回机制是 M 特权操作
2. **读写 M-mode CSR 在 U-mode**：所有 0x3xx / 0x7xx 编号的 CSR 需要 M 权限
3. **（保留）fence.i / sfence.vma**：6b 不涉及，留给 6c

**统一入口**：

```c
/* priv.c */
void priv_trap_illegal(word_t inst_bits, word_t epc) {
    trap_take(CAUSE_ILLEGAL_INST, inst_bits, epc);
}

/* inst.c 的 mret INSTPAT 改成： */
INSTPAT("0011000 00010 00000 000 00000 1110011", "mret", N, {
    if (priv_get() != PRIV_M) {
        priv_trap_illegal(s->inst, s->pc);
    } else {
        trap_mret(&s->dnpc);
    }
});
```

**CSR 指令加权限检查**：把 `csr_read / csr_write` 改成能拒绝：

```c
/* csr.h */
typedef enum { CSR_OK, CSR_PRIV_FAULT, CSR_NOT_EXIST } csr_status_t;

csr_status_t csr_access_check(uint32_t addr, bool write);

/* csr.c */
csr_status_t csr_access_check(uint32_t addr, bool write) {
    /* CSR 编号高 2 位 [11:10] 决定 R/W：0b11 = 只读；
     * 位 [9:8] 决定最低权限：M=0b11 / S=0b01 / U=0b00。 */
    uint32_t min_priv = (addr >> 8) & 3u;
    if (priv_get() < min_priv) return CSR_PRIV_FAULT;
    if (write && ((addr >> 10) & 3u) == 3u) return CSR_PRIV_FAULT;
    return CSR_OK;
}

/* inst.c 的 csrrw/csrrs/csrrc 统一前置： */
INSTPAT("??????? ????? ????? 001 ????? 1110011", "csrrw", I, {
    uint32_t addr = BITS(s->inst, 31, 20);
    if (csr_access_check(addr, true) != CSR_OK) {
        priv_trap_illegal(s->inst, s->pc);
    } else {
        word_t old = csr_read(addr);
        csr_write(addr, R(rs1));
        R(rd) = old;
    }
});
```

> **规范依据**：RISC-V Privileged Spec §2.1 "Each CSR has a 12-bit address. By convention, the upper 4 bits of the CSR address (csr[11:8]) encode the read and write accessibility and the privilege level..."

### 3.5 `mtval` 登场

6a 里 `trap_take` 收了 `tval` 但丢弃。6b 补上 `mtval` CSR（0x343），illegal instruction 时**硬件写入触发异常的指令 bits**，handler 可以读出来知道是谁犯事。

加到 `csr.c` 的 table 里一行即可——整个基础设施已经备好。

---

## 4. Mechanism：三个 chunk 拆解

### 4.1 Chunk 6b-1：特权状态机 + mtval

**新文件**：`src/isa/riscv32/priv.c` + `local-include/priv.h`

```c
/* priv.h */
#define PRIV_U  0u
#define PRIV_S  1u
#define PRIV_M  3u

uint32_t priv_get(void);
void     priv_set(uint32_t p);
void     priv_reset(void);   /* called from cpu_init */

/* Raise an illegal instruction exception. Centralised so future
 * callers (CSR access check, mret-in-U, fence.i) share one entry. */
void priv_trap_illegal(word_t inst_bits, word_t epc);
```

**改动清单**：

| 文件 | 改动 |
|------|------|
| `local-include/csr.h` | 加 `CSR_MTVAL 0x343`；struct 加 `word_t mtval;` |
| `csr.c` | 表里加一行 `{ CSR_MTVAL, "mtval", &csr.mtval }` |
| `trap.c` | `trap_commit` 把 `pending.tval` 写进 `csr.mtval`；按 `current_priv` 填 `MPP` |
| `trap.c` | `trap_mret` 取 MPP → `current_priv`；MPP 清成 U |
| `reg.c` 的 `cpu_init` | 加 `priv_reset()` |

**验收**：
```
(temu) info c
  ... mtval 一项存在 ...
  ... mstatus 的 MPP 位正确更新 ...
```
手动测：main.c 前加一行 `priv_set(PRIV_U)` 后跑任意 ecall 程序，应看到 `mcause=8` 而非 `11`。**用后删掉**——这只是 chunk 级验证，真正的 U-mode 切换等 6b-3。

### 4.2 Chunk 6b-2：CSR 权限检查 + mret 权限检查

**新增的行为**：

```
U-mode 执行:
  csrrw t0, mtvec, t1    →  illegal inst (cause=2, mtval=原指令 bits)
  csrr  t0, mcycle       →  OK (0xb00 是 M-mode 只读，但 mcycle 不在 6b 表里，
                              要么不实现要么视作 M-only。按后者处理更保守)
  mret                   →  illegal inst
  ecall                  →  ecall_from_u (cause=8)
```

**改动**：`inst.c` 的 6 条 CSR INSTPAT 和 mret INSTPAT 套上 `csr_access_check / priv_get` 检查，违规调 `priv_trap_illegal`。

**新增测试 `tests/programs/illegal-inst.c`**：
```c
/* 先在 M-mode 把 mtvec 设到 handler，设 mstatus.MPP=U 和 mepc=&user_code,
 * 然后 mret 下沉到 U-mode。user_code 里做：
 *   (1) ecall  —— 期望 cause=8
 *   (2) 从 handler 回来后 U-mode 再执行 csrr t0, mtvec —— 期望 cause=2
 *   (3) 从 handler 回来后 U-mode 执行 mret —— 期望 cause=2
 * 三次都由 kernel handler 记录 cause；最后 ebreak halt，a0=观察到的 cause bitmap */
```

**验收**：`make test-prog PROG=illegal-inst EBREAK=halt` 退出码 = `(1<<8)|(1<<2)`（两种 cause 至少各出现一次）。

### 4.3 Chunk 6b-3：两层结构 demo

**目标**：一份裸机镜像，布局：

```
0x80000000  kernel 入口 (_start)
            初始化 mtvec、mscratch、准备 U-mode 栈
            填 mstatus.MPP=U、mepc=user_main
            mret → 下沉到 U-mode user_main
0x80010000  user 代码 + 栈
            ecall SYS_write → 打印字符
            ecall SYS_exit  → kernel 写 halt ret 并 ebreak
```

**kernel 的 trap handler**：

```asm
trap_entry:
    csrrw sp, mscratch, sp       # 切到 kernel 栈（mscratch 里放 kernel sp）
    sw  t0, 0(sp)
    sw  a0, 4(sp)
    sw  a1, 8(sp)
    sw  a7, 12(sp)
    ...
    csrr a0, mcause
    li   t0, 8
    beq  a0, t0, ecall_from_u
    # ... 其他 cause 打印后 halt ...

ecall_from_u:
    li   t0, 1                   # SYS_write
    beq  a7, t0, do_write
    li   t0, 93                  # SYS_exit
    beq  a7, t0, do_exit
    j    unknown_syscall

do_write:
    # a0=fd (忽略), a1=buf, a2=len
    # 循环 sb 到 UART MMIO 地址 0xa00003f8
    ...
    li   a0, 0
    j    trap_return

do_exit:
    # a0=exit_code；切回 M-mode 然后 ebreak halt
    ...

trap_return:
    csrr t1, mepc
    addi t1, t1, 4               # 跳过 ecall 本身
    csrw mepc, t1
    lw  t0, 0(sp)
    ...                          # 恢复 GPR
    csrrw sp, mscratch, sp
    mret
```

**User 程序**：

```c
/* tests/programs/umode-hello.c */
static int sys_write(int fd, const char *buf, int len) {
    register int a0 asm("a0") = fd;
    register const char *a1 asm("a1") = buf;
    register int a2 asm("a2") = len;
    register int a7 asm("a7") = 1;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7));
    return a0;
}

static void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = 93;
    asm volatile ("ecall" : : "r"(a0), "r"(a7));
    __builtin_unreachable();
}

void user_main(void) {
    sys_write(1, "hello from U-mode\n", 18);
    sys_exit(0);
}
```

**验收命令**：

```bash
make test-stage6b
# Expected:
#   stdout: "hello from U-mode"
#   exit code 0
#   difftest clean
```

---

## 5. Concrete example — 一次 `ecall` 全流程（6b 视角）

```
初始状态（kernel 刚 mret 到 U-mode）：
  current_priv = U
  mstatus.MIE  = 1 (允许中断，尽管 6b demo 没开定时器)
  mstatus.MPP  = U（规范要求 mret 后 MPP 自动变 U）
  pc = 0x80010040 (user_main)
  sp = user_stack

user_main 执行到 ecall:
  INSTPAT 命中 ecall →
    priv_get() == U → cause = 8 (CAUSE_ECALL_U)
    trap_take(8, 0, s->pc=0x80010048)

isa_exec_once 返回；cpu_exec 看到 trap_pending：
  trap_commit:
    csr.mepc     = 0x80010048
    csr.mcause   = 8
    csr.mtval    = 0
    csr.mstatus.MPIE = mstatus.MIE (=1)
    csr.mstatus.MIE  = 0
    csr.mstatus.MPP  = current_priv (=U)
    current_priv = M
    cpu.pc = csr.mtvec (=trap_entry)

下一条 exec_once 在 M-mode 取指 trap_entry:
  csrrw sp, mscratch, sp  →  sp = kernel_sp, mscratch = user_sp
  保存 a0/a1/a2/a7 到 kernel 栈
  csrr a0, mcause = 8 → 进入 ecall_from_u
  a7 = 1 → do_write
  循环把 buf 里的字节写到 0xa00003f8 (UART) → 终端打印

trap_return:
  mepc += 4 (跳过 ecall 指令)
  恢复寄存器
  csrrw sp, mscratch, sp  →  切回 user sp
  mret:
    current_priv = mstatus.MPP = U
    mstatus.MIE = MPIE = 1
    mstatus.MPP = U
    cpu.pc = mepc = 0x8001004c

user 程序从 ecall 之后继续跑，看到 a0 = 字节数。
```

---

## 6. Acceptance

| Chunk | 验收命令 | 预期 |
|-------|---------|------|
| 6b-1 | `make run` 后 `info c` | 看到 `mtval` 一行 |
| 6b-1 | 改 `main.c` 临时 `priv_set(PRIV_U)` 跑现有 `trap-ecall.bin` | `mcause=8` |
| 6b-2 | `make test-prog PROG=illegal-inst EBREAK=halt` | 退出码包含 cause=2 和 cause=8 各一次 |
| 6b-2 | `make test` | 旧的 48 isa + 6 program + 6a 全绿 |
| 6b-3 | `make test-stage6b` | stdout "hello from U-mode"，exit 0 |
| 6b-3 | `make test-stage6a` | 回归绿（定时器中断仍正常）|

---

## 7. Limitations / pitfalls

### 7.1 `current_priv` 的存放位置

**不要** 塞进 `CPU_state`——CSR 和 priv 是"处理器模式"的一部分，和 GPR/PC 的"体系结构可见状态"分层不同。和 CSR 一样放独立模块。**但 reset 要统一**：`cpu_init` 里一起重置，否则冷启动态不确定。

### 7.2 测试用例的 "模式切换"

6b 之前所有测试都假设 M-mode。**不要一刀切把默认改成 U**——现有 `.S` 测试没 kernel，没 mtvec，下沉 U-mode 后第一条 CSR 操作就 illegal inst 连锁。正确做法：默认启动保持 M-mode，**新测试自己负责**下沉到 U-mode（通过设 MPP+mepc+mret 的标准套路）。

### 7.3 CSR 权限检查的"副作用时机"

`csrrw rd, csr, rs1` 的正确语义：**先读出 CSR 旧值**，**再写入 rs1 值**，**最后把旧值塞 rd**。如果权限检查失败，**所有副作用都不能发生**（包括 rd 写入）。所以顺序是：

```c
1. addr = BITS(inst, 31, 20)
2. if (!csr_access_check) { trap; return; }   /* 注意此时 pending_trap 已标 */
3. old = csr_read(addr)
4. csr_write(addr, new)
5. R(rd) = old                                 /* rd=x0 由末尾统一清零兜底 */
```

踩坑版：先写了 rd 再检查权限，U-mode 程序 `csrrw a0, mstatus, zero` 会把 mstatus 值泄漏到 a0 再 trap——信息泄漏漏洞的教科书样例。

### 7.4 `csrrs / csrrc` 的 "rs1=0 特殊语义"

规范要求：`csrrs/csrrc` 当 `rs1=x0` 时**不**执行 CSR 写侧——只读。权限检查也要跟上：**只读时检查 R 权限，不检查 W 权限**。

```c
bool will_write = (rs1 != 0);
if (csr_access_check(addr, will_write) != CSR_OK) { trap; return; }
```

否则 U-mode 程序想 `csrr t0, mcycle`（read-only probe）会被误拒。

### 7.5 `mret` 之后 MPP 必须强制清到 U

规范明文："If xRET is executed, xPP is set to the least-privileged supported mode (U if U-mode is supported)"。如果不清零，连续两次 `mret` 会把同一个 MPP 反复用，形成 "特权泄漏" ——栈应该只有一层深。

### 7.6 kernel trap handler 必须 `csrrw sp, mscratch, sp`

用户态 sp 指向用户栈，直接 `sw ra, 0(sp)` 会把用户栈写坏，更糟的是用户栈可能根本没映射（6c 加 MMU 后这是必现 segfault）。标准套路是**进入 handler 第一条**就 `csrrw sp, mscratch, sp` 把 kernel sp 换进来、把 user sp 甩到 mscratch；**退出前最后一条**再换回去。

这是**规范层面就暗示的协议**——`mscratch` 存在就是干这个的。

### 7.7 difftest 的 privilege 对齐

我们的 ref 实现也要有自己的 `current_priv`。否则 trap/mret 时两侧 mstatus.MPP 不一致，difftest 报假 mismatch。实现上：ref 直接 `priv_get()` 共享全局（两实现都住在同一个进程里），或给 ref 自己一份同步的 `ref_priv`——后者更干净。

### 7.8 `ecall` 不是 "系统调用"

RISC-V 里 `ecall` 只是**发起 environment call 这件事本身**。"哪个 syscall" 由 a7 寄存器编码，完全是**软件约定**（ABI）。硬件不管。6b demo 里我们自定义 a7=1/93，和 Linux 的 `SYS_write=64/SYS_exit=93` **只碰巧 exit 对上**——不要误以为是兼容 Linux。

### 7.9 WFI 在 6b 要不要实现

规范里 `WFI` 是 M/S-mode 指令，U-mode 执行看 `mstatus.TW` 位：TW=0 允许，TW=1 illegal inst。我们暂时**不实现 TW**，把 WFI 在任何模式当 no-op（实际真机它就是"等中断的空循环"）。写一行注释标好。

---

## 8. 与后续 stage 的连接

| Stage 6b 留下的 | Stage 6c/6d 会怎么用 |
|-----------------|----------------------|
| `current_priv` 全局 | 6c 的 MMU：取指 / load / store 都要传 `current_priv` 看 PTE 的 U 位 |
| CSR 权限检查框架 | 6c 加 S-mode CSR（sstatus/satp/...）只是表里多几行 |
| `priv_trap_illegal` 入口 | 6c 的 page fault (`CAUSE_INST_PAGE_FAULT=12`) 复用同一条 trap 路径 |
| mscratch swap 协议 | 6d 的 context switch：trap 进来先 swap，再 sw 所有 GPR 到 task_struct |
| illegal-inst 测试 | 6c 加 `ACCESS_FAULT` / `PAGE_FAULT` 时做相同风格的单元测试 |

---

## 9. 与 OS 课的连接

| 概念（用户笔记）| Stage 6b 对应实现 |
|-----------------|---------------------|
| 「用户态 vs 核心态」| `priv_get() == PRIV_U` 的分支 |
| 「系统调用号 in a7」| `ecall_from_u` 里 `beq a7, t0, do_write` |
| 「内核栈 / 用户栈分离」| `csrrw sp, mscratch, sp` |
| 「上下文保存」| handler 头几行的 `sw t0, 0(sp) ...` |
| 「权限违规 → trap」| `priv_trap_illegal` 统一入口 |
| 「MPP 是硬件自动栈的一层」| `trap_commit` 写 MPP、`trap_mret` 读 MPP |

**取舍标签**：theoretical ideal。这个 stage 的工作量小（~300 行新代码），但**概念密度高**：MPP、MPRV（保留）、CSR 编号的 [11:8] 权限位、ecall-from-X 的 cause 分化——全是"OS 课讲了两周但一看代码 10 行"的典型。完成后你会知道 Linux kernel 里的 `SYSCALL_DEFINE3` 宏为什么要写成那样。

---

## 10. 不做清单（写给未来的我）

6b **不包含**以下内容，留到后续 stage：

- PMP / PMA（物理内存保护）→ 跳过，6c 直接上 MMU
- S-mode CSR（sstatus / sepc / stvec / satp）→ 6c
- `medeleg / mideleg` 异常/中断委托给 S-mode → 6c
- `mstatus.MPRV`（借用 MPP 权限做 load/store）→ 6c 需要
- `mstatus.SUM / MXR`（S-mode 访问 U-pages / X-only 可读）→ 6c
- WFI 的 `mstatus.TW` 超时机制 → 不做
- 浮点 CSR（`mstatus.FS`、fcsr）→ 不做（我们连 F 扩展都没做）

每条都在 CSR 表或指令里**留好接入点**（比如 mstatus 的位段已经定义全）——添加时不用回头改结构。
