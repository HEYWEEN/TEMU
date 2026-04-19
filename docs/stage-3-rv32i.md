# Stage 3 — RV32I 指令解码与执行

> **状态**：📝 设计稿
> **验收命令**：`make test-prog`（跑通 `dummy.c` 和 `fib.c`）

---

## 1. Problem motivation

Stage 0-2 我们搭起了「壳 + 调试器 + 内存 + 寄存器」。Stage 3 要让这些东西**真的动起来**：按照 RV32I 规范，把内存里的 32 位字**当作指令**依次执行，改变 CPU 状态。

> 这是整个项目的**理论核心**——之前的 stage 都是"工具准备"，之后的 stage 都是"工程扩展"，只有 stage 3 是"让它真的是一台 CPU"。

---

## 2. Naive solution & why not

> 一个巨型 `switch(opcode)` 分发到每条指令的 `case`。

RV32I 有 ~40 条指令、7 种指令格式。naive 实现会长成这样：

```c
switch (opcode) {
    case 0x13:    // OP-IMM 家族
        switch (funct3) {
            case 0: // ADDI
                if (rd != 0) gpr[rd] = gpr[rs1] + imm_I;
                break;
            case 1: // SLLI
                ...
            case 5: // SRLI / SRAI 靠 funct7 区分
                if (funct7 == 0) ...
                else ...
        }
        break;
    case 0x33:    // OP 家族
        ...
    // 几百行 nested switch
}
```

**问题**：

1. **难读**：指令的编码位模式被拆成 opcode/funct3/funct7 三次分支，读代码时脑内要拼回 32 位
2. **易错**：改一个 funct3 表忘了 funct7，编译器不会警告
3. **难写测试**：指令格式的解码逻辑散落在各个 case 里
4. **和 ISA 手册不对应**：手册是「每条指令一行位模式」的表，代码应该长得像那张表

---

## 3. Key idea — Pattern-match 宏

> 让代码**长得像 ISA 手册里的表格**。

NEMU 的做法（本项目抄用）：

```c
#define INSTPAT(pattern, name, type, ...)                             \
    do {                                                              \
        uint32_t key, mask, shift;                                    \
        pattern_decode(pattern, STRLEN(pattern), &key, &mask, &shift);\
        if (((INSTPAT_INST(s) >> shift) & mask) == key) {             \
            INSTPAT_MATCH(s, name, type, ## __VA_ARGS__);             \
            goto *(__instpat_end);                                    \
        }                                                             \
    } while (0)

// 使用：
INSTPAT_START();
INSTPAT("??????? ????? ????? 000 ????? 00100 11", addi, I,
        R(rd) = src1 + imm);
INSTPAT("0000000 ????? ????? 000 ????? 01100 11", add,  R,
        R(rd) = src1 + src2);
INSTPAT("0100000 ????? ????? 000 ????? 01100 11", sub,  R,
        R(rd) = src1 - src2);
// ...
INSTPAT_END();
```

**效果**：每条指令一行，模式串 = 规范里的位模式，行动 = 一个 C 表达式。

> **关键洞察**：`?` 表示 don't care，其他都是常量位。`pattern_decode` 在编译期（或首次执行时）把模式串转成 `(key, mask)`，然后 `match` 就是 `(inst & mask) == key`。

---

## 4. Mechanism

### 4.1 主执行循环

```c
void cpu_exec(uint64_t n) {
    for (uint64_t i = 0; i < n && !halt_requested; i++) {
        Decode s;
        s.pc      = cpu.pc;
        s.snpc    = cpu.pc + 4;           // static next PC
        s.inst    = paddr_read(s.pc, 4);  // 取指

        decode_exec(&s);                  // 解码 + 执行（可能改 dnpc）
        cpu.pc = s.dnpc;                  // 写回 PC

        cpu.gpr[0] = 0;                   // x0 硬连线 0
        check_watchpoints();              // 监视点
    }
}
```

**`snpc` vs `dnpc`**：
- **snpc** = static next PC = `pc + 4`（顺序执行情况下）
- **dnpc** = dynamic next PC = 可能被跳转 / 分支改写

顺序指令里 `dnpc = snpc`；跳转指令里 `dnpc` 被指令重写。默认初始化为 `snpc`，只有 JAL/JALR/Bxx 才覆盖。

### 4.2 指令格式与立即数编码（⚠️ 最易错）

RV32I 六种指令格式：

| 格式 | 用途 | 立即数组成 |
|------|------|-----------|
| **R** | 寄存器-寄存器 ALU（ADD / SUB / AND ...）| 无立即数 |
| **I** | 立即数 ALU、LOAD、JALR | `inst[31:20]` 符号扩展 |
| **S** | STORE | `{inst[31:25], inst[11:7]}` 符号扩展 |
| **B** | 条件分支 | `{inst[31], inst[7], inst[30:25], inst[11:8], 0}` 符号扩展（**注意最低位补 0**） |
| **U** | LUI / AUIPC | `{inst[31:12], 12'b0}` |
| **J** | JAL | `{inst[31], inst[19:12], inst[20], inst[30:21], 0}` 符号扩展 |

**B-type 和 J-type 的立即数位拼装顺序是 RV32I 最大坑点**。写错不会编译报错，但分支会跳到错地址。

> **工程 hack**：写一个 immI / immS / immB / immU / immJ 的宏 / 函数库，所有指令都调用它们，别在每条指令里现拼位。

```c
#define SEXT(x, n) ((int32_t)((x) << (32 - (n))) >> (32 - (n)))
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi)-(lo)+1)) - 1))

#define immI(i) SEXT(BITS(i, 31, 20), 12)
#define immS(i) SEXT((BITS(i,31,25)<<5) | BITS(i,11,7), 12)
#define immB(i) SEXT((BITS(i,31,31)<<12) | (BITS(i,7,7)<<11) | \
                     (BITS(i,30,25)<<5) | (BITS(i,11,8)<<1), 13)
#define immU(i) (BITS(i,31,12) << 12)
#define immJ(i) SEXT((BITS(i,31,31)<<20) | (BITS(i,19,12)<<12) | \
                     (BITS(i,20,20)<<11) | (BITS(i,30,21)<<1), 21)
```

### 4.3 操作数解码

```c
static void decode_operand(Decode *s, int type,
                           int *rd, word_t *src1, word_t *src2, word_t *imm) {
    uint32_t i  = s->inst;
    int rs1     = BITS(i, 19, 15);
    int rs2     = BITS(i, 24, 20);
    *rd         = BITS(i, 11, 7);
    switch (type) {
        case TYPE_R: *src1 = R(rs1); *src2 = R(rs2); break;
        case TYPE_I: *src1 = R(rs1); *imm  = immI(i); break;
        case TYPE_S: *src1 = R(rs1); *src2 = R(rs2); *imm = immS(i); break;
        case TYPE_B: *src1 = R(rs1); *src2 = R(rs2); *imm = immB(i); break;
        case TYPE_U: *imm  = immU(i); break;
        case TYPE_J: *imm  = immJ(i); break;
    }
}
```

### 4.4 指令清单（按类别）

#### 算术逻辑（寄存器-寄存器 / 寄存器-立即数）
`ADD SUB SLL SLT SLTU XOR SRL SRA OR AND`
`ADDI SLLI SLTI SLTIU XORI SRLI SRAI ORI ANDI`

#### 上立即数
`LUI AUIPC`

#### 分支
`BEQ BNE BLT BGE BLTU BGEU`

#### 跳转
`JAL JALR`

#### Load / Store
`LB LH LW LBU LHU SB SH SW`

#### 系统（stage 3 先停机即可）
`ECALL EBREAK`

#### FENCE（stage 3 可当 NOP）
`FENCE FENCE.I`

### 4.5 几个容易写错的指令细节

| 指令 | 坑 |
|------|----|
| **LB / LH** | 读出后**符号扩展**到 32 位 |
| **LBU / LHU** | 读出后**零扩展** |
| **JALR** | 目标地址 = `(rs1 + imm) & ~1`（**末位清零**） |
| **SRA / SRAI** | 算术右移，需要先转 `int32_t` 再移 |
| **SUB vs ADD** | 靠 `funct7` 区分（`0100000` vs `0000000`） |
| **SLT vs SLTU** | 前者 signed 比较，后者 unsigned |
| **Bxx 偏移** | 相对 `pc`，不是相对 `pc+4` |

### 4.6 目录组织

```
src/isa/riscv32/
├── reg.c                      # 寄存器别名表 + 打印
├── inst.c                     # INSTPAT 表 + decode_exec
├── local-include/
│   ├── inst.h                 # Decode 结构、INSTPAT 宏
│   └── reg.h                  # 寄存器索引宏
```

---

## 5. Concrete example — 走读 `addi x1, x0, 5`（编码 `0x00500093`）

```
二进制: 0000 0000 0101 0000 0000 0000 1001 0011
        └ imm[11:0] ┘ └rs1┘ fun└ rd┘ └opcode┘
                 5    0    0   1    0010011
```

1. 取指：`inst = paddr_read(pc, 4) = 0x00500093`
2. INSTPAT 匹配到 `addi` 行：`??????? ????? ????? 000 ????? 0010011`
3. `decode_operand(TYPE_I)`：`rd=1`, `src1=R(0)=0`, `imm=5`
4. 执行：`R(rd) = src1 + imm` → `R(1) = 0 + 5 = 5`
5. `snpc = pc + 4`（顺序指令），`dnpc = snpc`
6. 写回 PC，清 x0

跑完后 `info r` 看 `ra (x1) = 5`。

---

## 6. Acceptance

### 6.1 单指令测试（tests/isa/）

每条指令写一个 `.S` 片段，汇编后用 `objcopy -O binary` 抽出 `.text`，用一个测试 harness 加载 + 执行 + 检查结果。

```asm
# test-addi.S
.global _start
_start:
    addi x1, x0, 5
    addi x2, x1, 10
    ebreak                  # 停机
```

harness：加载此 binary，`cpu_exec(-1)`，断言 `gpr[1] == 5`，`gpr[2] == 15`。

### 6.2 程序测试（tests/programs/）

```c
// fib.c
int fib(int n) { return n < 2 ? n : fib(n-1) + fib(n-2); }
int main(void) { return fib(10); }   // 应返回 55
```

交叉编译：

```bash
riscv64-unknown-elf-gcc -march=rv32i -mabi=ilp32 -nostdlib -nostartfiles \
    -T link.ld tests/programs/fib.c tests/programs/crt0.S -o fib.elf
riscv64-unknown-elf-objcopy -O binary fib.elf fib.bin
```

然后 `./build/temu fib.bin`，CPU 跑到 `ecall` / `ebreak` 时读 `a0` (`x10`) 检查返回值。

### 6.3 Makefile target

```makefile
test-isa:
	@./tests/isa/run-all.sh

test-prog: $(TARGET)
	@for prog in tests/programs/*.bin; do \
		echo "==> $$prog"; ./build/temu -b $$prog || exit 1; done
```

---

## 7. Limitations / pitfalls

### 7.1 没有异常机制（stage 6 才加）

`ecall` / `ebreak` 在 stage 3 的处理：直接设 `halt_requested = true` 并把 `a0` 的值记下来作为"退出码"。正规的 trap 机制要等 stage 6。

### 7.2 没有 CSR（stage 6 才加）

RV32I 规范里有 CSR 指令（`CSRRW` / `CSRRS` ...）。stage 3 如果没跑特权态程序，可以不实现——crt0.S 别写 CSR 指令就行。

### 7.3 `M` / `A` / `F` / `D` / `C` 扩展全不实现

- `M`（乘除）：`mul / div` 不做，fib 程序不需要乘除也能跑
- `A`（原子）：单核无需
- `F` / `D`（浮点）：停一下。浮点是另一个项目
- `C`（压缩）：指令变成 16/32 变长，取指复杂度爆炸，先不动

编译 C 程序时 `-march=rv32i` 会禁用所有扩展。

### 7.4 分支目标对齐

RV32I 要求分支目标**4 字节对齐**（未实现 C 扩展时）。如果 `dnpc` 不对齐，规范说要触发 instruction-address-misaligned 异常。stage 3 先 `panic`，stage 6 改成异常。

### 7.5 有符号 vs 无符号

C 语言里有符号溢出是 UB。RV32I 里 ADD 的溢出是**无符号回绕**。所以所有算术都用 `uint32_t`（`word_t`）运算，只有在需要"有符号比较"时才临时转 `int32_t`。

```c
// SLT:
R(rd) = ((int32_t)src1 < (int32_t)src2) ? 1 : 0;
// SLTU:
R(rd) = (src1 < src2) ? 1 : 0;
```

### 7.6 Trace 日志

Stage 3 开始引入 `CONFIG_ITRACE`（instruction trace）：每条指令执行后往环形 buffer 里写一条反汇编结果。程序崩溃时打印最近 16 条。**这个对 stage 4 debug 救命。**

---

## 8. 与后续 stage 的连接

| Stage 3 留下的东西 | 谁会用它 |
|-------------------|----------|
| `cpu_exec(n)` | Stage 4 的 difftest 用它逐步走 |
| `decode_exec` | Stage 4 对比寄存器后 tested |
| ITRACE | Stage 4 bug 复现时打印上下文 |
| `halt_requested` 机制 | Stage 5 串口收到特定字节时用来优雅停机 |
| INSTPAT 表 | Stage 6 加系统指令时在同一张表扩充 |

---

## 9. 为什么这个 stage 这么长

因为这是**把 ISA 规范翻译成代码**的过程——每条指令都是一个小的精确契约，大意不得。

建议节奏：**一次只写一条指令，写完就跑它的单元测试**。全写完再测 = 几百个 bug 一起出来，没法定位。
