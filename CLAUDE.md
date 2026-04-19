# CLAUDE.md — Terminal 计算机模拟器项目

> 本文件是项目级别的工作指令，交给后续进入此目录的 Claude Code 阅读。
> 用户 yiwenhe 的全局配置（中文回复、学习/开发模式规则）继续生效。

---

## 0. 项目是什么

在终端里实现一个**运行在宿主计算机上的简化计算机**，参考南京大学 NEMU (NJU EMUlator)。

**一句话定位**：写一个 C 程序，它把另一个"裸机"ISA 的字节序列当作状态机来执行。

**核心心智模型**（来自用户的 OS 笔记）：

```c
struct computer {
    CPU_state cpu;             // 寄存器 + PC
    uint8_t   pmem[PMEM_SIZE]; // 物理内存
    // 设备、MMIO 映射等
};
```

> 进程 = CPU 状态 + 可访问内存。模拟器做的事就是：在 C 里手搓这个状态机，然后在一个大循环里迁移它。

---

## 1. 语言与工具链（硬性约束）

| 项 | 值 | 理由 |
|----|-----|-----|
| 语言 | **C11**，纯 C | 用户指定；贴近系统层，没有魔法 |
| 构建 | GNU Make | 简单、可控；不要上 CMake / Bazel |
| 宿主 | macOS (darwin)，兼容 Linux | 用户是 macOS |
| 编译器 | `clang` 优先，`gcc` 兼容 | macOS 默认 |
| 依赖 | 仅 libc；可选 `readline`（命令行历史）| 零依赖优先 |
| 语言版本 | `-std=c11 -Wall -Wextra -Werror` | 严格，能早报错就早报 |

**绝对禁止**：C++、Rust、Python、shell 脚本以外的任何高级语言。外部库在未经用户确认前不引入。

---

## 2. ISA 选择：RISC-V 32I

**选它而不是 x86 的理由**：

| 维度 | RV32I | x86 |
|------|-------|-----|
| 核心指令数 | ~40 | 数百（加上扩展上千）|
| 指令编码 | 定长 32-bit，字段规整 | 变长 1–15 字节，前缀地狱 |
| 文档 | 开源 PDF，免费 | 手册三大卷 |
| 工具链 | `riscv64-unknown-elf-gcc` 一条 `brew install` | 交叉编译繁琐 |
| 学习价值 | 现代 RISC 教科书级设计 | 历史包袱重 |

**写死在项目里**：先只做 `rv32i` 用户态整数指令集，不做 M/A/F/D/C 扩展。等基本 CPU 能跑 helloworld 再考虑。

---

## 3. 目录结构

```
.
├── CLAUDE.md                # 本文件
├── Makefile                 # 顶层构建
├── README.md                # 给人看的简介
├── include/                 # 公共头文件
│   ├── common.h             # 基本类型、Log、panic
│   ├── cpu.h                # CPU_state、cpu_exec
│   ├── memory.h             # pmem_read/write
│   ├── isa.h                # ISA 抽象（未来可换架构）
│   └── debug.h              # Assert、Trace 开关
├── src/
│   ├── main.c               # 入口；参数解析；启动 REPL
│   ├── monitor/             # 调试器
│   │   ├── sdb.c            # REPL 主循环、命令分派
│   │   ├── expr.c           # 表达式求值（lexer + parser）
│   │   └── watchpoint.c     # 监视点
│   ├── memory/
│   │   └── pmem.c           # 物理内存数组 + 读写
│   ├── cpu/
│   │   └── cpu_exec.c       # 主执行循环
│   ├── isa/
│   │   └── riscv32/
│   │       ├── reg.c        # 寄存器文件 + 别名
│   │       ├── inst.c       # 指令解码 + 执行
│   │       └── local-include/inst.h
│   └── utils/
│       ├── log.c            # 日志实现
│       └── disasm.c         # （可选）反汇编
├── tests/
│   ├── expr/                # 表达式测试：每行 "期望值  表达式"
│   ├── isa/                 # 单指令汇编测试
│   └── programs/            # 完整小程序（fib、string-reverse...）
└── tools/
    └── gen-expr/            # 随机表达式生成器（用于 fuzz 测试求值器）
```

**原则**：`include/` 只放对外公共头；模块内部的头放 `src/<module>/local-include/`，不污染全局。

---

## 4. 分阶段实现（Stages）

> **铁律**：一个 stage 跑不通，就不准进入下一个。每 stage 结束要有一条 `make` 命令能验证。

### Stage 0：骨架与 REPL

**目标**：能编译、能进入空的 REPL、能 `q` 退出。

- `Makefile`：`all`、`clean`、`run`、`test` 四个 target
- `main.c`：解析 `-h`、`-b`（batch 模式）、`-l <log-file>`、镜像文件路径
- REPL：打印 banner → 读一行 → 分派 → 循环
- 只实现 `help` 和 `q` 两个命令

**验收**：`make run` 进入提示符；输入 `q` 干净退出。

### Stage 1：Monitor / 简易调试器

**目标**：一个能操作"尚未存在的 CPU"的壳。

命令表（命名参考 GDB）：

| 命令 | 作用 |
|------|------|
| `help [cmd]` | 帮助 |
| `c` | continue 直到结束或命中断点 |
| `si [N]` | 单步执行 N 条（默认 1）|
| `info r` | 打印寄存器 |
| `info w` | 打印监视点 |
| `x N EXPR` | 从 EXPR 起扫描 N 个 4 字节 |
| `p EXPR` | 求值表达式 |
| `w EXPR` | 新建监视点 |
| `d N` | 删除编号 N 的监视点 |
| `q` | 退出 |

**表达式求值** 必须支持：
- 十进制、十六进制、寄存器名（`$pc`、`$x0`..`$x31`、`$ra` 等别名）
- 运算符：`+ - * /`、`== != < > <= >=`、`&& || !`、`& | ^ ~`
- 解引用：`*EXPR`（读 4 字节）
- 括号、一元负号、优先级

**实现方式**：手写递归下降 parser，不要上 flex/bison。词法用 `regex.h` 或手写状态机都行。

**验收**：
- `tests/expr/` 跑全过
- `make test-expr` 绿

### Stage 2：内存 + 寄存器 + PC

- `pmem`：`static uint8_t pmem[PMEM_SIZE]`，默认 128MB（`0x80000000` 起，RISC-V 惯例）
- 接口：
  ```c
  uint32_t pmem_read(paddr_t addr, int len);  // len ∈ {1,2,4}
  void     pmem_write(paddr_t addr, int len, uint32_t data);
  ```
- 越界访问必须 `panic()`，不能静默
- `CPU_state`：`uint32_t gpr[32]; uint32_t pc;`
- 寄存器别名表（x0=zero, x1=ra, x2=sp, x5=t0, ...）
- 写 `x0` 必须**丢弃**（硬连线零寄存器）

**验收**：能从镜像文件 `memcpy` 到 `pmem[RESET_VECTOR]`；`info r` 正确打印 32 个寄存器。

### Stage 3：RV32I 指令解码与执行

**核心循环**：

```c
void cpu_exec(uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        uint32_t inst = pmem_read(cpu.pc, 4);
        cpu.pc += 4;
        decode_and_exec(inst);         // 可能再改 pc（跳转/分支）
        cpu.gpr[0] = 0;                // 每条指令后清零 x0（偷懒但正确）
        if (triggered_watchpoint()) break;
    }
}
```

**解码**：用 **pattern-match 宏** 而不是巨型 switch。参考 NEMU 的做法：

```c
#define INSTPAT(pattern, name, type, ...) \
    if (match_pattern(inst, pattern)) { \
        decode_operand_##type(inst, ...); \
        exec_##name(__VA_ARGS__); \
        matched = true; \
    }

INSTPAT("??????? ????? ????? 000 ????? 0010011", addi, I, ...);
INSTPAT("0000000 ????? ????? 000 ????? 0110011", add,  R, ...);
// ...
```

**覆盖 rv32i 全部**：
- 算术逻辑：ADD/SUB/ADDI/AND/OR/XOR/ANDI/ORI/XORI/SLL/SRL/SRA/SLLI/SRLI/SRAI/SLT/SLTU/SLTI/SLTIU
- 分支：BEQ/BNE/BLT/BGE/BLTU/BGEU
- 跳转：JAL/JALR
- Load/Store：LB/LH/LW/LBU/LHU/SB/SH/SW
- 上立即数：LUI/AUIPC
- 系统：ECALL/EBREAK（先停机处理即可）

**验收**：跑通 `tests/programs/dummy.c`（一个空 main）和 `fib.c`（返回 fib(10)）。

### Stage 4：Difftest（差分测试）

**目标**：每执行一条指令，把 CPU 状态跟参考实现比对；不一致立刻停。

- 参考实现：Spike (`riscv-isa-sim`) 或自写的"第二套"实现
- 接口抽象：`ref_init / ref_exec_one_step / ref_regcpy / ref_memcpy`
- 命中不一致：打印 PC、预期 vs 实际寄存器 diff、立即 `panic`

**为什么必须做**：写 ISA 不可能一次写对。没有 difftest，bug 会在跑程序时以"奇怪崩溃"呈现，调试成本爆炸。

### Stage 5：设备 / MMIO

- 串口：一个 memory-mapped 地址，写入即 `putchar(data & 0xFF)`
- 定时器：64-bit 自增计数器，从 0 开始，只读
- 通过 `mmio_add_map(addr, len, callback)` 注册；`pmem_read/write` 路由判断地址归属

### Stage 6（可选）：异常、分页、跑一个小 OS

等前面都稳了再说。此时可以引入用户在 OS 笔记里学的：`_start`、`execve`、地址空间、重定位。

---

## 5. 编码规范

### 命名
- 文件 / 函数 / 变量：`snake_case`
- 宏 / 常量：`UPPER_SNAKE`
- 结构体：`typedef struct { ... } CPU_state;`（模块级公共类型）
- 不用匈牙利命名

### 函数组织
- 一个 `.c` 对应一个 `.h`（或内部放 `local-include/`）
- 跨模块调用只走公共头
- 静态函数一律 `static`

### 错误处理
```c
#define Assert(cond, fmt, ...) \
    do { if (!(cond)) { \
        fflush(stdout); \
        fprintf(stderr, "\33[1;31m" fmt "\33[0m\n", ##__VA_ARGS__); \
        assert(cond); \
    }} while (0)

#define panic(fmt, ...) Assert(0, fmt, ##__VA_ARGS__)
```

- 不变式违反 → `Assert`
- 外部输入错误（非法镜像、越界访问）→ `panic`
- **不要写 try-catch 风格的错误传播**，C 里没那个东西

### 日志
- `Log(fmt, ...)`：带文件、行号、颜色，写 stderr
- `Trace` 系列按开关：`CONFIG_ITRACE`（指令）、`CONFIG_MTRACE`（内存）、`CONFIG_FTRACE`（函数）
- 默认都关，靠 `Makefile` 变量或 `config.h` 打开

### 绝对不要做的事

1. **不要在早期 stage 引入后期才需要的抽象**（比如 stage 3 就写"未来换 ISA 用的接口层"）
2. **不要用 `malloc` 管 CPU 状态 / pmem**，用静态全局
3. **不要 mock 内存或寄存器**，它们是真实的 C 数据
4. **不要一次性写完所有指令再测试**，一条一条写、一条一条跑
5. **不要把解码写成巨型 switch-case**，用 pattern-match
6. **不要写"可能以后用得到"的 helper**，YAGNI
7. **不要加中文注释的 docstring**，行内注释只在"为什么"非显然时写

### 必须做的事

1. 每新增一条指令 → 写（或跑）一个对应测试
2. 每改完一个 stage → `make && make test-<stage>` 全绿再提交
3. 查规范：`RISC-V Unprivileged ISA Manual`（当前最新 v20240411）
4. 提交信息用英文祈使句（`add JALR decode`、`fix sign-ext in LB`）

---

## 6. 测试

### 三档测试

| 档 | 位置 | 跑法 |
|----|------|------|
| 表达式求值 | `tests/expr/` | `make test-expr` |
| 单指令 | `tests/isa/` | `make test-isa` |
| 程序级 | `tests/programs/` | `make test-prog` |

### 表达式测试格式
```
# tests/expr/basic.txt
3           1+2
10          2 * (3 + 2)
1           1 == 1
0           1 && 0
```

### 单指令测试
- 用 `riscv64-unknown-elf-as` 汇编 `.S` → 提取 `.text` → 二进制喂给模拟器
- 预置初始寄存器，跑一条，检查结果

### 程序测试
- 小 C 程序 + 最小 crt0.S → 链接 → `objcopy` 出 raw binary
- 模拟器加载并运行，检查返回码或输出

---

## 7. 参考资料

### 用户已有 OS 笔记（直接映射到本项目概念）

| 概念 | 笔记位置（`/Users/heyween/HEYWEEN.github.io/_posts/`）|
|------|---------|
| 状态机模型（CPU + 内存）| `2026-03-18-程序与进程.md` |
| 地址空间、虚拟/物理地址 | `2026-03-26-进程的地址空间.md`、`2026-04-14-存储管理.md` |
| ELF / execve / 链接 | `2026-04-08-链接和加载.md` |
| `_start` 与 C runtime | `2026-03-07-应用视角的操作系统.md` |
| 指令集、指令周期 | `2025-12-09-指令系统.md`、`2025-12-10-指令周期和指令流水线.md` |

### 外部权威

- **NJU ICS PA 讲义**：https://nju-projectn.github.io/ics-pa-gitbook/ics2024/
- **NEMU 源码**：https://github.com/NJU-ProjectN/nemu
- **RISC-V 规范**：`riscv-unprivileged.pdf`（搜 `riscv isa manual`）
- **Spike 参考实现**：https://github.com/riscv-software-src/riscv-isa-sim
- **RV32I 指令表速查**：https://mark.theis.site/riscv/

---

## 8. 与用户 yiwenhe 的协作约定

1. **用户身份**：CS 方向在校生，有 C / OS / 计组基础；实践经验偏少 → 解释贴近 PA 讲义风格，不要过度工程化
2. **语言**：中文回复；代码、命令、术语保留英文
3. **每完成一个 stage** 必须报告：
   - ✅ 改了什么（哪些文件 / 对应哪个 stage）
   - 🧪 怎么测（一条可复制的 `make` 命令）
   - ➡️ 下一步建议
4. **不确定就问**：需求边界模糊、有两种可行设计时，先问再做
5. **不要跳 stage**：即使你看到最终目标是什么，也按阶段推进
6. **合理质疑**：如果用户的某个要求跟本文档冲突，指出来一起讨论，不闷头执行

---

## 9. 起步第一步（给后续 Claude Code）

读完本文件后：

1. 检查目录是否真的为空（`ls -la`）
2. 创建 `Makefile` 骨架 + `src/main.c` 空 REPL
3. 跑 `make run`，确认能编译、能进入提示符、能 `q` 退出
4. 报告 stage 0 完成，等用户说进入 stage 1

**不要一次把整个项目写完。** 这违反本文件第 4、5、8 节的所有约定。
