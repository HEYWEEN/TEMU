# TEMU — TErminal Machine Emulator

> 一台能在终端里跑起来的简化计算机。宿主 macOS / Linux，目标 ISA RISC-V 32I。用 C 手写，参考 [NJU NEMU](https://github.com/NJU-ProjectN/nemu)。

---

## 一、这是什么

一个 C 程序，它**把另一台机器的字节序列当作状态来执行**。

核心数据结构就两样东西：

```c
struct computer {
    CPU_state cpu;             // 寄存器 + PC
    uint8_t   pmem[PMEM_SIZE]; // 物理内存
};
```

把这结构放进一个循环，每轮做「取指 → 解码 → 执行 → 推进 PC」，就是一台能跑的计算机。课堂上讲的「程序 = 状态机 + 迁移函数」，在这里会从抽象口号变成一段实际可跑的 C。

---

## 二、为什么做这个项目

- **操作系统**的「进程 = CPU 状态 + 地址空间」不再是 PPT 词汇，而是你自己定义的 `struct`
- **指令集架构**不再是手册里的表格，而是你必须拆解每一位的 bit field
- **编译器 / 链接器**的产物（ELF、重定位、`_start`）不再神秘，因为你要自己加载并解释它
- 最后能看到自己手写的 CPU 跑一个 `fib(10)` 并输出正确结果，这个反馈是其它作业给不了的

---

## 三、路线图

每个 stage **跑不通就不准进入下一个**。每个 stage 都有一条可复制的 `make` 命令验收。

| Stage | 状态 | 目标 | 验收 | 详细说明 |
|-------|------|------|------|----------|
| 0 | ✅ | 骨架 + REPL | `make run` 进入提示符，`q` 干净退出 | [stage-0-skeleton.md](docs/stage-0-skeleton.md) |
| 1 | ✅ | Monitor / 调试器 + 表达式求值器 | `make test-expr` 全绿 | [stage-1-monitor.md](docs/stage-1-monitor.md) |
| 2 | ✅ | 物理内存 + 寄存器 + PC | `info r` 打印 32 个寄存器 | [stage-2-memory-cpu.md](docs/stage-2-memory-cpu.md) |
| 3 | ✅ | RV32I 指令解码与执行 | 递归 fib、迭代 fib 等程序 | [stage-3-rv32i.md](docs/stage-3-rv32i.md) |
| 4 | ✅ | Difftest 差分测试 | 两套实现 52 例 lock-step 无差异 | [stage-4-difftest.md](docs/stage-4-difftest.md) |
| 5 | ✅ | MMIO 设备（串口、定时器）| hello world + 单调定时器测试 | [stage-5-mmio.md](docs/stage-5-mmio.md) |
| 6 |   | （可选）异常 / 分页 / 小 OS | 跑起一个用户态进程 | [stage-6-os.md](docs/stage-6-os.md) |

**上手指南**：[docs/playground.md](docs/playground.md) —— 克隆下来后怎么"玩"，REPL 游 + 手搓程序 + 看 difftest 工作。

详细阅读建议与文档写作约定见 [docs/README.md](docs/README.md)。

---

## 四、快速开始

```bash
# 克隆并进入目录
cd TEMU

# 编译
make

# 启动 REPL（Stage 0 只认识 help / q）
make run

# 批量模式 + 指定日志文件 + 指定镜像（Stage 0 镜像暂未加载）
./build/temu -b -l run.log path/to/image.bin

# 清理
make clean
```

**注意**：macOS 自带的 `getopt` 不 permute，选项必须放在位置参数**之前**。即写 `./temu -b -l log.txt image.bin`，而不是 `./temu image.bin -b`。

---

## 五、目录结构（随 stage 推进增长）

当前（Stage 0）：

```
.
├── CLAUDE.md              # 给 Claude Code 的项目级工作指令
├── README.md              # 本文件
├── Makefile
├── docs/                  # 每个 stage 的教科书式说明
│   ├── README.md
│   ├── stage-0-skeleton.md
│   └── stage-1-monitor.md ...
├── src/
│   └── main.c             # 入口 + 参数解析 + REPL
└── build/                 # 编译产物（.gitignore'd）
```

最终形态参见 [CLAUDE.md § 3](CLAUDE.md)。

---

## 六、设计取舍速览

| 取舍点 | 选择 | 为什么 |
|-------|------|--------|
| 语言 | C11 纯 C | 贴近系统层，没有魔法；和模拟的目标（裸机程序）同构 |
| 目标 ISA | RV32I | 40 条核心指令、定长 32-bit、文档免费、工具链 brew 可得 |
| 构建 | GNU Make | 简单可控；不要上 CMake / Bazel |
| 依赖 | 仅 libc（可选 readline）| 零依赖优先，不让包管理分散注意力 |
| 内存 | 静态数组 128MB | 不 `malloc`；模拟器启动即所有资源就位 |
| 解码 | pattern-match 宏 | 巨型 switch-case 不可维护，pattern 表接近 ISA 手册表达 |
| 调试 | 自写 GDB 风格调试器 | 顺带逼自己实现表达式求值器（学 parser 的好借口）|

**不选 x86 的原因**：变长指令 1–15 字节 + 前缀地狱 + 手册三大卷。RV32I 是现代 RISC 的教科书级设计。

---

## 七、对开发者 / 读者的约定

- **阅读顺序**：README（本文） → `docs/README.md` → 当前 stage 文档 → `CLAUDE.md`（深度工程规范）
- **提交信息**：英文祈使句，如 `add JALR decode` / `fix sign-ext in LB`
- **每完成一个 stage** 必须给出：改了什么 / 怎么测 / 下一步建议
- **不要跳 stage**：即使看得到最终目标，也按阶段推进。跳 stage 会让 bug 定位成本指数上升

---

## 八、参考资料

| 资源 | 用途 |
|------|------|
| [NJU ICS PA 讲义](https://nju-projectn.github.io/ics-pa-gitbook/ics2024/) | 本项目最重要的对标；阶段划分和许多设计抄自 PA |
| [NEMU 源码](https://github.com/NJU-ProjectN/nemu) | 参考实现，卡住时对照 |
| `riscv-unprivileged.pdf` (v20240411) | RV32I 权威规范，解码疑问查这个 |
| [Spike](https://github.com/riscv-software-src/riscv-isa-sim) | 差分测试的参考实现（Stage 4 用到）|
| [RV32I 速查](https://mark.theis.site/riscv/) | 写指令时的速查表 |

---

## 九、License

个人学习项目，暂未设 License。后续若公开再补。
