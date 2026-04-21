---
title: "P5 · I/O 设备与 MMIO：guest 与 host 的第一条通路"
date: 2026-07-10
categories: [计算机系统]
tags: [MMIO, Port-IO, UART, Serial, Timer, CLINT, mtimecmp, Device-Model, RV32I]
math: true
---

## 本章目标

读完本章你能回答：

- CPU + 内存 = 完整计算机？差哪一块？
- 为什么 RISC-V、ARM 都选 MMIO，而 x86 保留了 port I/O？
- "设备"在模拟器里到底是什么？一个让很多人意外的答案
- 30 行的 `serial.c` 为什么代表 **guest 第一次能对 host 说话**？
- `timer` 为什么要**同时**暴露一个 MMIO 地址**和**一个 `timer_mtime()` C 函数——这不是重复吗？
- 为什么 `paddr_touched_mmio` 这个 flag 一次要服务三个章节（P2 声明、P4 使用、P6a 激活）？

**动手做到**：

- 把 P2 的 `paddr_read/write` 从"只看 pmem 数组"推成"先看 pmem、再查 MMIO 表"
- 写一个新设备（比如 debug 寄存器）挂到地址 0xa000_0100，靠它 `Log` 自己的执行
- 跑通 `prog.c` 里的 `test_serial_hello`——guest 程序第一次把 `hello\n` 送到宿主 stdout
- 跑通 `test_timer_monotonic`——验证连续两次读 mtime 单调递增

**本章不涉及**：

- **DMA / 总线仲裁**——真硬件关心，模拟器上 callback 同步执行就够了
- **中断 delivery 路径**——P6a 主场；本章只铺 timer 底座，不讲 MTIP 怎么转成 `mip.MTIP`
- **用户态设备隔离 / 驱动框架**——Stage 6b 谈完分页再说
- **真·16550 UART 建模**——QEMU 做这个，TEMU 只做单字节输出

---

## 1. 问题动机：CPU + 内存 ≠ 计算机

P2 给了 pmem，P3 装了 CPU，P4 用差分测试证明这台 CPU 对 RV32I spec 忠实。此时你跑：

```
$ ./build/temu -b hello.bin
[pmem.c:43 paddr_write] out of bound: addr=0xa00003f8 len=1
Abort trap: 6
```

等等。`hello.bin` 只是六个 `sb` 指令，把 `'h' 'e' 'l' 'l' 'o' '\n'` 写到 `0xa00003f8`。为什么 panic？

因为你的 CPU 是**关起门的**。pmem 范围是 `[0x80000000, 0x88000000)`——128MB。`0xa00003f8` 不在里面，所以 `paddr_write` 直接 panic（src/memory/pmem.c:59）：

```c
panic("paddr_write out of bound: addr=0x%08" PRIx32 " len=%d", addr, len);
```

这是对的。P2 的合约说过："越界访问必须 panic，不能静默"。可这导致一个荒谬的现实——**你手上这台 CPU 算得过来 fib(10)，却打不出 `hello world`**。

> ⚠️ **常见误解**：很多人以为"写模拟器 = 实现 ISA"。错。一台 CPU 不能打字、不能看时间、不能感知外界，它就只是个状态机玩具。**I/O 是让计算机配得上"计算机"这个词的最后一块拼图**。

本章要做的事：把 `[0x88000000, ∞)` 这片"越界区"的一小段，重新定义成**设备寄存器**。写到这些地址，不再 panic，而是触发宿主那边的副作用——比如 `putchar`，比如读 wall clock。

---

## 2. 两种 I/O 范式：为什么 RISC-V 选 MMIO

硬件和 CPU 交换数据，历史上就两条路。

### 2.1 Port I/O（x86 血脉）

独立的 **I/O 地址空间**，16 位端口号，共 64K 个。专用指令：

```asm
in  al, 0x3f8     ; 从串口读一个字节
out 0x3f8, al     ; 往串口写一个字节
```

硬件实现：CPU 有一根独立的 `M/IO#` 信号线，高=访问内存，低=访问 I/O。地址译码器按这根线分流。

### 2.2 MMIO（RISC-V / ARM / 现代 MIPS 血脉）

**设备寄存器被映射到主内存地址空间**的某段。访问它们用 **普通 load/store 指令**：

```asm
lui  a0, 0xa0000     ; a0 = 0xa0000000
addi a0, a0, 0x3f8   ; a0 = 0xa00003f8
sb   t0, 0(a0)       ; *(uint8_t*)0xa00003f8 = t0  ← 输出字符
```

CPU 根本不知道 `0xa00003f8` 是"设备"。它发了一个 bus cycle 到那个地址，**地址译码器**决定这次访问路由给 DRAM 控制器还是 UART。

### 2.3 RISC-V 的选择

| 维度 | Port I/O (x86) | MMIO (RISC-V / ARM) |
|------|----------------|---------------------|
| ISA 新指令 | `IN`/`OUT` 一族 | **0 条** |
| 驱动代码 | `inb(0x3f8)` / `outb(0x3f8, c)` 特殊 API | `*(uint8_t*)0xa00003f8 = c;` 普通指针 |
| 编译器优化 | 编译器不敢优化 I/O 指令 | 要 `volatile` 告诉编译器别合并 |
| 硬件复杂度 | 多一条 `M/IO#` 线 + 独立译码器 | 统一译码，地址一视同仁 |
| 地址空间 | 独立 64K port space | 吃主内存地址 |
| 设备数量上限 | 65536 个 port | 受地址空间限制（RV32 下 4GB，绰绰有余） |

> **核心洞察**：MMIO 抛弃了 "I/O 是一种特殊操作" 的分类。硬件和软件都因此变简单——**ISA 正交性的胜利**。代价：内存地图要明确划出一块区域给设备用，写错地址会访问到设备寄存器而不是 DRAM。x86 兼容性包袱太重没法改，才保留了 port I/O。

RV32I 里根本找不到 `IN`/`OUT` 这类指令。这不是"RISC-V 忘了做 I/O"，是"I/O 根本不需要新指令"。

### 2.4 历史视角：MMIO 从 PDP-11 的 Unibus 来

MMIO 不是 RISC-V 的发明，是个 **50 年** 的老想法。

**1970 - PDP-11 + Unibus**（DEC 公司的 Gordon Bell 主导设计）：

PDP-11 的 16-bit 地址空间里，最上面的 `0o170000` – `0o177777`（即 0xF000 – 0xFFFF 的 4KB）被硬件规定为 **I/O 页**——任何设备控制器（磁盘、磁带、终端、时钟）的寄存器都挂在这段地址上。CPU 没有 IN/OUT 指令，一切 I/O 就是 `MOV #char, @#177564`（写控制台发送寄存器）。

**关键创新**：Unibus 让**一条总线**同时承载内存访问和设备访问，**由地址本身决定走哪条路**。DRAM 控制器和每个设备控制器都监听同一根总线，各自响应自己被分配的地址段。这就是现代 MMIO 的**物理原型**——CPU 不知道对面是什么，地址译码器决定一切。

> Bell 后来说："Unibus 最重要的东西不是它的性能，是它让**加设备变简单**——接到总线上、分配地址段、写驱动完事。"（1979 访谈）

**为什么这是革命性的**：之前 IBM/CDC 大机都走"Channel I/O"——专用 I/O 处理器走专用协议。PDP-11 把 I/O **降级成一次普通内存访问**，设备驱动不再需要和 Channel 控制器打交道，直接用 `MOV`。整个操作系统的 I/O 路径缩了 10 倍代码量。

**1975 – Motorola 6800 / 6502**：继承 PDP-11 思想，完全**没有**I/O 指令族。8-bit 微机时代把 MMIO 普及到 Apple II、Commodore 64 这些家用机上——屏幕、键盘、磁带口全都是某个内存地址。

**1985 – MIPS R2000**：学术界第一次把 MMIO 写进 RISC 指令集手册的"正统做法"——没有 I/O 专用指令，load/store 管一切。

**1990s – ARMv4 / ARMv7**：嵌入式世界彻底 MMIO。STM32、树莓派、iPhone 芯片，没有一个有"I/O 指令"这个东西。

**2014 – RISC-V**：spec 里连"I/O 指令"这个词都不出现——MMIO 是**默认假设**，甚至不需要讨论。

**x86 的"化石 port I/O"**：x86 至今保留 IN/OUT，原因是 1978 年 Intel 8086 当时还在模仿 PDP-8 风格的独立 I/O 空间；1982 年 80286 已经可以用 MMIO，但为了运行 DOS 程序只能保留向后兼容。现代 x86 驱动（尤其是 Linux 内核）**99% 走 MMIO**，`inb` / `outb` 几乎只在老键盘控制器、传统串口（COM1）这种上古设备上出现。

**⚠️ 常见误解**：以为 MMIO 是"现代发明、RISC 才有"。反了——**MMIO 是 1970 年的主流**，port I/O 反而是 Intel 为兼容老 8080 / Z80 保留的历史特例。

### 2.5 volatile 和 cache coherence：MMIO 的两个隐藏陷阱

§2.3 的表里有一行写 "MMIO 要 `volatile` 告诉编译器别合并"。这句话对 TEMU 这种纯模拟器无关痛痒（我们没有编译器优化也没有 cache），但真实 MMIO 代码是**被这两件事反复坑过**的。

**陷阱 1：编译器优化会吃掉 MMIO 访问**

考虑一段 C 代码（某串口驱动）：

```c
while (*(uint8_t*)0xa00003fd & 1)    /* 等 "TX 空" bit */
    ;
*(uint8_t*)0xa00003f8 = ch;         /* 发字符 */
```

不加 `volatile`，**优化器会把 `*0xa00003fd` 的读提出循环外**——因为编译器按纯内存模型推理："循环里没人写这个地址，值不会变"。结果：程序永远死循环，或永远不检查状态直接写数据。

解法：

```c
#define UART_STATUS   (*(volatile uint8_t*)0xa00003fd)
#define UART_TX       (*(volatile uint8_t*)0xa00003f8)
while (UART_STATUS & 1) ;
UART_TX = ch;
```

`volatile` 告诉编译器：**每次访问都必须真实发出 load/store**，不得合并、不得 reorder、不得提出循环。它的语义就是为 MMIO / signal handler / setjmp 设计的。

**常犯错误**：`volatile uint8_t *ptr = ...`（指针本身 volatile）和 `uint8_t *volatile ptr = ...`（指针指向的目标 volatile）混淆。MMIO 要的是后者。

**陷阱 2：Cache 会让 MMIO 写看起来"延迟生效"**

现代 CPU 的 L1 data cache 默认 **write-back** 模式——`sb 0xa00003f8, t0` 这条指令并**不立刻**把字节推给 UART，而是先写进 L1 cache line。cache line 被换出（或显式 flush）时才真正上总线。

问题：UART 一直看不到你写的字符，直到某天 cache 碰巧被换出——行为"偶发有效"。

解法（真实硬件上）：

1. **MMU 页属性**：把 MMIO 地址段映射为 "strongly ordered" / "device memory" / "uncached"，硬件就不经 cache 直接打总线（ARMv8 的 `Device-nGnRE`、RISC-V Zicbom + PBMT 的 `MT = IO`）
2. **显式屏障**：`fence`（RISC-V）、`dsb`（ARM）、`mfence`（x86）——保证前面的 store 全部可见再继续
3. **write-through cache**：小嵌入式 CPU 常用，性能差但行为直观

**对 TEMU 的意义**：

- TEMU 没有 cache，`paddr_write` 调用就等于"bus 事务完成"——我们绕过了这个坑
- 但**真实 RISC-V 芯片上跑 TEMU 生成的程序**，`sb ..., 0xa00003f8` 不配合 `fence` 和页属性，串口字符发不出去——这是从模拟器跨到真硬件第一个撞墙的地方
- 练习 Hard 里"加 cache" 就会把这个问题暴露出来

**连接**：`volatile` 控制**编译器层**的 reorder；`fence` 控制**硬件层**的 reorder；**两者正交**，都要。这是 C11 `atomic` 和 memory model 讨论的前奏——现代多核同步绕不开这个二元组。

---

## 3. 核心洞察：设备 = 带副作用的地址段

回忆 P2 `paddr_write(0x80001000, 1, 0x68)` 的语义——**把 0x68 写到 pmem 数组第 0x1000 字节**。纯粹内存操作，没有副作用。

现在考虑 `paddr_write(0xa00003f8, 1, 0x68)`。语义是什么？

**"把 0x68 送给宿主 stdout，让屏幕上出现一个 `h`。"**

两条指令长得一模一样，行为天差地别。差别只在地址落在哪一段。

> **核心洞察**：**设备在模拟器里就是挂在某段地址上的 C 回调函数**。"访问这个地址 → 跑这段 C" —— 就这么简单。模拟器里没有什么"设备总线"、"PCI 配置空间"、"中断控制器"的神秘结构，它们最终都是一张 `addr → callback` 的表。
>
> 这不是模拟器的偷懒。这是**硬件的真实模型抽象**。真实的 UART 就是一块硅片，CPU 的地址线到它那里就触发了 TX 状态机。区别只是——那边是晶体管，这边是 C 函数。**C 函数就是一个慢一万倍的 UART 芯片**。

有了这个模型，下面的实现就只是"怎么优雅地维护这张表"。

---

## 4. `paddr_read/write` 的双路径分派

看 P5 结束后 `paddr_read` 的完整形态（src/memory/pmem.c:30）：

```c
word_t paddr_read(paddr_t addr, int len) {
    Assert(len == 1 || len == 2 || len == 4,
           "paddr_read: bad length %d", len);

    if (in_pmem(addr) && in_pmem(addr + (paddr_t)len - 1)) {
        return pmem_read(addr, len);          /* 99% 命中 */
    }
    if (mmio_in_range(addr)) {
        word_t data = 0;
        mmio_access(addr, len, false, &data);
        paddr_touched_mmio = true;            /* P4 埋的伏笔，P5 点亮 */
        return data;
    }
    panic("paddr_read out of bound: addr=0x%08" PRIx32 " len=%d", addr, len);
}
```

### 4.1 三个设计要点

**(1) pmem 优先，MMIO 次之**

`in_pmem` 就是两次 `<`，比扫 MMIO 注册表快。让最常见路径（load/store DRAM）零开销通过。

**(2) panic 仍然保留**

越界访问**不给默认值**。如果未来你忘了注册某设备就访问它，panic 立刻告诉你"这个地址没人管"。相比 "读返回 0、写静默" 的宽松实现，这样 bug 暴露得早。

**(3) `paddr_touched_mmio` 这个 flag**

P2 里只声明（`bool paddr_touched_mmio`）没用。P4 在 `difftest_step` 里检查它（决定要不要 snapshot 而不是 lockstep 比对）。P5 在这里**第一次把它置 true**——"这条指令碰过 MMIO"。

> **洞察**：一个 `bool` 服务三个章节。代码里这种 **跨章节伏笔** 是 TEMU 的设计特点之一：早声明、晚使用、一次到位，避免后期 retrofit。你也可以反过来看这是"分布式状态"的臭味——任何读它的模块都隐式依赖其他模块正确 set/clear 它。工程上是 trade-off，教学上值得拿出来讲。

### 4.2 为什么不用一个统一的地址范围表？

朴素方案是把 pmem 和 MMIO 都挂进同一张 `mmio_add_map` 表：pmem 也注册成一段 callback。代价：每次访问 pmem 都要扫表——**hot path 被拖成线性查找**。

| 方案 | pmem 访问 | MMIO 访问 | 代码行数 |
|------|-----------|-----------|----------|
| 现方案：pmem 硬编码 + MMIO 查表 | `if` 两次比较 | 线性扫最多 8 项 | +5 行 |
| 统一查表 | 线性扫 + 第 0 项是 pmem | 同样线性扫 | 代码更短 |

选硬编码是 **pragmatic choice**——教学代码里清晰度 > 抽象对称性。等你真写工业级 VMM 再做 radix tree 查表也不迟。

---

## 5. MMIO 注册表：极简设计

`src/device/mmio.c` 全部不到 60 行。API：

```c
typedef void (*mmio_cb_t)(paddr_t off, int len, bool is_write, word_t *data);

void mmio_add_map(paddr_t lo, paddr_t hi, mmio_cb_t cb, const char *name);
bool mmio_in_range(paddr_t addr);
void mmio_access  (paddr_t addr, int len, bool is_write, word_t *data);
```

几个**设计决策** 值得拆开看。

### 5.1 为什么不是动态数组？

```c
#define MAX_MMIO 8
static mmio_map_t map[MAX_MMIO];
static int        nr_map = 0;
```

对比 Linux：动态注册、按需增长、数百个驱动。TEMU 写死 8 个槽位就够——serial、timer、mtimecmp，加上未来可能的键盘、VGA、磁盘，压根到不了 8。YAGNI 原则：**不要为"可能用到"的抽象付代价**。

### 5.2 注册时验证重叠

```c
for (int i = 0; i < nr_map; i++) {
    Assert(!(lo < map[i].hi && hi > map[i].lo),
           "MMIO region '%s' overlaps with '%s'", name, map[i].name);
}
```

两段 `[lo1, hi1)` 和 `[lo2, hi2)` 不相交 ⇔ `hi1 <= lo2 || hi2 <= lo1`。取反加德摩根律 → `lo1 < hi2 && hi1 > lo2`。assert 即早发现。

**为什么不检查和 pmem 重叠？** 因为 pmem 在 `[0x80000000, 0x88000000)`，MMIO 约定挂在 `0xa000_0000` 附近——两区域隔了 384MB 空白。assert 里不检查是"信任约定"的一种表达。未来若扩大 pmem 到 `0x90000000` 才要加这条检查。

### 5.3 callback 的"合一"签名

最朴素的设计会给每个设备写两个回调：`read_cb` 和 `write_cb`。TEMU 把它们合成一个，用 `is_write` 标志分派。

| 方案 | 好处 | 坏处 |
|------|------|------|
| 合一（`is_write` 标志） | 读写共享状态天然在一起看得见 | callback 内部多一次 `if` |
| 分离（两个函数指针） | 每个函数职责单一 | 读写共享状态要放 file-static 跨函数 |

TEMU 选合一**主要是为 timer 考虑**——mtimecmp 的读写要共同维护那个 64-bit 变量，放一个函数里更紧凑（timer.c:62）。serial 里那个 `if` 显得多余，但统一的签名让所有设备长一个样，认知成本低。

### 5.4 偏移而不是绝对地址

```c
map[i].cb(addr - map[i].lo, len, is_write, data);
```

callback 拿到的 `off` 是**相对该段 base 的偏移**，不是绝对地址。好处：设备代码里不用知道自己被挂在哪个 base——未来想把 serial 从 `0xa00003f8` 换到 `0xa000_5000` 只改 `mmio_add_map` 一行，`serial_cb` 一个字节不改。

**经典 locality 原则**：信息该在哪层知道，就只让那层知道。

### 5.5 长度检查

```c
Assert(addr + (paddr_t)len <= map[i].hi, ...);
```

防止 `lw` 跨越两段 MMIO。比如你 `lw a0, 0(0xa00003f7)`——4 字节读 `[0xa00003f7, 0xa00003fb)`——前 1 字节是 serial，后 3 字节悬空。assert 让这种错误立刻暴露。

---

## 6. serial：第一个设备，只有 30 行

`src/device/serial.c` 全部：

```c
#define SERIAL_BASE 0xa00003f8u
#define SERIAL_SIZE 8

static void serial_cb(paddr_t off, int len, bool is_write, word_t *data) {
    (void)off;
    (void)len;
    if (is_write) {
        putchar((int)(*data & 0xff));
        fflush(stdout);
    } else {
        *data = 0;
    }
}

void init_serial(void) {
    mmio_add_map(SERIAL_BASE, SERIAL_BASE + SERIAL_SIZE, serial_cb, "serial");
}
```

真的就这么多。读完你就能写第二个设备。几个值得拆的细节：

### 6.1 `0xa00003f8` 这个地址哪来的

1981 年 IBM PC 把串口 UART 16550 的端口号定在 `0x3f8`（COM1）。之后整个 x86 世界都记得这个数字。TEMU 取 `0xa000_0000 | 0x3f8` 是致敬——给老程序员一点熟悉感，虽然 RISC-V 上完全没有"COM1"这个概念，是装饰性选择。

你也可以随便选别的，比如 `0xa000_0000` 整数。不影响任何程序。

### 6.2 为什么忽略 `off` 和 `len`

真 16550 有 8 个寄存器：THR（发送数据）、RBR（接收数据）、IER（中断使能）、LCR（线控）、... `off` 选哪一个。TEMU 的 serial **只有一个寄存器**（输出），所以：

- `off=0..7` 全部映射到同一个写入行为（"forgiving mode"——程序不管写到哪个偏移都被接受）
- `len=1/2/4` 都只取 `*data & 0xff` ——`sw` 发四字节，只输出低字节

这是"玩具级 UART"。想做 16550 完整建模参考 QEMU 的 `hw/char/serial.c`——900+ 行。

### 6.3 `fflush(stdout)` 必须每次调

```c
putchar((int)(*data & 0xff));
fflush(stdout);           // ← 看似多余，实则关键
```

stdout 默认是**行缓冲**（交互式）或**全缓冲**（pipe）。如果你的 guest 程序写了 `"hello"` 就 `ebreak` 退出，没写 `'\n'`——libc 里缓冲区的 5 个字符**永远不会刷出来**。

> ⚠️ **常见误解**："我明明写了 `putchar('h')`，为什么屏幕上没东西？" 答案：缓冲。解决：每次 flush（或测试时用 `stdout = unbuffered`）。

真硬件没这问题——串口线是物理的，电平一变就发出去了。模拟的"串口"借宿主 libc 的 stdout，就得懂 libc 的缓冲规则。

### 6.4 读返回 0 的语义

```c
else {
    *data = 0;
}
```

真 16550 的 RBR 若有输入数据返回它，没数据时……行为取决于 LSR 状态位。TEMU 没建模输入路径，所以读永远返回 0。程序如果设计了"轮询到读出非零就当作 input"，配合 TEMU 会永远卡住——但目前 TEMU 上没有这种程序，够用。

想加 stdin 参考本章末练习 3。

### 6.5 验收

`tests/programs/prog.c:240` 的 `test_serial_hello`：

```c
RUN_OUT("serial hello", 0, "hello",
    /* a0 = 0xa00003f8 */
    LUI(A0, 0xa0000000),
    ADDI(A0, A0, 0x3f8),
    /* h e l l o \n */
    ADDI(T0, ZERO, 'h'), SB(T0, A0, 0),
    ADDI(T0, ZERO, 'e'), SB(T0, A0, 0),
    ADDI(T0, ZERO, 'l'), SB(T0, A0, 0),
    ADDI(T0, ZERO, 'l'), SB(T0, A0, 0),
    ADDI(T0, ZERO, 'o'), SB(T0, A0, 0),
    ADDI(T0, ZERO, '\n'), SB(T0, A0, 0),
    ADDI(A0, ZERO, 0));
```

跑 `make test-prog`——你看见 `hello` 从 stdout 出来。这是 guest 机器 **第一次把信息送出它自己的地址空间**。

在此之前，所有状态都困在 `cpu.gpr[]` 和 `pmem[]` 里——就算 `fib(20)` 算对了，你也得靠 halt 时的 `a0` 或手工 `x` 命令才能看到结果。现在它**直接能对 host 说话**。

这个瞬间比任何新指令都更接近"计算机"这个词的本意。

### 6.6 历史视角：UART 从 RS-232 到 16550

TEMU 的 30 行 serial 设备是 **1960–1995 年** 一系列规范堆叠而成的结果。每一层都解决了前一层的某个实际工程痛点。

**1960 – RS-232（Bell Labs / EIA）**：

**问题**：电传打字机（teletype）和主机怎么通信？当时没"串口"概念，各家厂商自己定义电平、协议。RS-232 统一了电气接口：

- ±12V 差分信号（`-12V` = 逻辑 1，`+12V` = 逻辑 0，反直觉来自当年硬件约束）
- 9 根信号线（DB-9 连接器）：TX、RX、RTS、CTS、DTR、DSR、DCD、RI、GND
- **异步**：没有共享时钟，靠 start bit 对齐；数据以 1 起始 + 5–8 数据 + 1 停止的帧发送
- 速率商量：300 / 1200 / 9600 / 19200 bps，两边必须约定一致

RS-232 是**机电规范**——只管物理层。**怎么把 CPU 寄存器变成串行比特流**？需要专门芯片。

**1971 – UART 概念（WE 2502）**：

Western Electric 把"CPU 并行写一个字节 → 芯片自己产生 start bit/停止位/奇偶校验，按时钟移位输出"**做成一块硅片**。这就是 **U**niversal **A**synchronous **R**eceiver **T**ransmitter 的由来。

CPU 看到的接口（抽象）：

```
TX_DATA  寄存器：写字节，芯片自动串出
TX_READY 状态位：0 = 忙，1 = 空，可再写
RX_DATA  寄存器：读进来的字节
RX_READY 状态位：有数据未读？
```

**1977 – National Semiconductor 8250**：

第一个主流廉价 UART 芯片，IBM PC（1981）用了它当 COM1/COM2。8250 把上面 4 个寄存器扩成 10 个，加了 baud rate divisor、interrupt enable、modem control。成了**事实标准**——DOS 下所有串口程序按 8250 寄存器模型写。

**1987 – 16550（NS16550A）**：

**问题**：8250 每收到一字节就中断一次。PC 主频升到 16 MHz 后，486 系统跑满速串口（115200 bps）每秒被中断 1 万次，OS 光处理中断就卡。

**16550 的解法**：加 **16 字节 FIFO** 硬件队列。数据先进 FIFO，FIFO 半满（可配）才中断一次——中断率降到 1/8 到 1/16，CPU 解放。

寄存器地图（这就是 TEMU 用的 0x3F8–0x3FF 那 8 个字节）：

| offset | 寄存器 | 用途 |
|--|--|--|
| 0 | RBR / THR | 读：receive buffer；写：transmit holding |
| 1 | IER | 中断使能 |
| 2 | IIR / FCR | 读：中断识别；写：FIFO 控制 |
| 3 | LCR | line control（数据位、校验、停止位）|
| 4 | MCR | modem control |
| 5 | LSR | line status（TX 空、RX 满、错误）|
| 6 | MSR | modem status |
| 7 | SCR | scratch register |

**2026 年的今天**：16550 已经 39 岁，但它的寄存器布局**在 Linux / FreeBSD / 所有 BSD 内核的 `ns16550.c` 驱动里字节级原样保留**——甚至 ARM 嵌入式 SoC 和树莓派的 UART IP 都**自称** "16550-compatible"，因为换一块寄存器模型就意味着整个软件栈跟着换，没人敢动。

**TEMU 的极简版**：

- 只建模"offset 0 写入 = 送字符给 stdout"
- 其余 7 个寄存器读返回 0、写忽略
- FIFO / interrupt / modem control 一律不做

这是因为我们**复用 host stdout**——host 的 libc 和终端内核已经做了所有 FIFO / 流控工作，TEMU 不需要重建。**30 行 vs QEMU 的 `hw/char/serial.c` 的 900 行**——差在"玩具"和"工业"之间，但思想内核是一样的：一块挂在某个地址上的状态机。

**⚠️ 常见误解**：

- "RS-232 = UART = 串口" —— 三个层：RS-232 是**电气**，UART 是**芯片抽象**，"串口"是**连接器习惯叫法**。三者可解耦——蓝牙 SPP 走 UART 接口但用无线电替代 RS-232
- "16550 是老古董应该淘汰" —— 反了，它是**最成功的 ABI** 之一。换了就和"在 x86 上废除 int 0x80 系统调用"一样——理论上可以，实际上没人愿意付迁移成本

**连接**：这段历史解释了**为什么 TEMU 的 `0xa00003f8` 地址看起来"突然冒出来"**——`0x3f8` 是 1981 年 IBM PC 的 COM1 端口号（port I/O 地址），spike 和 QEMU 继承这个习惯，把它映射到 MMIO 地址段的尾部。**你写的 serial 驱动仍然在和 1981 年的 IBM 对齐**。

---

## 7. timer：第一个**真正有状态**的设备

serial 是**无状态**的——数据一出去就忘，下次进来和上次无关。timer 不同：它有一个**随时间变化的值**，而且这个变化**不由 CPU 触发**。

### 7.1 两个子设备合一个文件

`src/device/timer.c` 里注册了两段 MMIO：

```c
#define TIMER_BASE    0xa0000048u    /* mtime     低 4 + 高 4 = 8 字节 */
#define MTIMECMP_BASE 0xa0000050u    /* mtimecmp  低 4 + 高 4 = 8 字节 */
```

- **mtime**（只读）：自启动起的微秒数
- **mtimecmp**（读写）：软件设的"到这个时间点产生中断"

两者加起来**就是 RISC-V CLINT（Core Local Interruptor）规范的最小子集**。真 CLINT 还有 `msip`（软件中断）、按 hart 分组等，TEMU 单核不需要。

### 7.2 wall clock vs instruction count：时间到底怎么来？

实现 `now_us()` 有两种完全不同的哲学：

| 策略 | 语义 | 好处 | 坏处 |
|------|------|------|------|
| **Wall clock**（宿主真实时间） | `gettimeofday()` | `sleep 1 秒`的程序真的睡 1 秒；用户体验一致 | **不可复现**——两次运行 timer 读数不同；速率依赖模拟器有多快 |
| **Instruction count** | 执行指令数 × 假想周期 | **完全可复现**；和 ISA 一样是确定性系统 | 真实时钟和模拟时钟脱节；`sleep 1s` 变成忙等 10^9 条指令 |

TEMU 选 wall clock（timer.c:33）：

```c
static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

uint64_t timer_mtime(void) {
    return now_us() - boot_time_us;
}
```

为什么？**Stage 6a 的定时器中断 demo 想让"每秒触发一次"真的是每秒**——不是每 10^9 条指令。对教学演示这是 pragmatic 选择，代价由差分测试吸收：

> **连 P4**：`paddr_touched_mmio = true` 导致 `difftest_step` 跳过比对、snapshot ref 状态。timer 的不可复现性被**系统级**吸收，不用在 timer 里自己处理。

### 7.3 mtimecmp 的 64-bit 写入 hazard

真 CLINT 和 TEMU 都把 64-bit mtimecmp 暴露成两个 32-bit MMIO 寄存器（RV32 ISA 没有 64-bit 存储指令，只能分两次 `sw`）。timer.c:62 的注释指出了 hazard：

```
writing the low word first can momentarily create a stale threshold
that trips the interrupt immediately — matches real CLINT behaviour,
which is why Linux writes the high word first when disarming.
```

**场景**：当前 `mtimecmp = 0x0000_0001_ffff_ffff`（已过期很久了），软件想改成 `0x0000_0002_8000_0000`。

- **步骤 1**：写低 word `0x8000_0000` → 寄存器变成 `0x0000_0001_8000_0000`
- 此时 `mtime ≈ 0x0000_0002_1000_0000`，**已经大于 mtimecmp** → `mip.MTIP` 被置起
- **步骤 2**：软件刚打算写高 word，但中断已经触发，trap 处理器被调用——**比预期早了半辈子**

Linux RISC-V timer 驱动（drivers/clocksource/timer-riscv.c）的写法是：

```
1. 先写高 word 为 UINT32_MAX —— 保证 mtimecmp 绝对在未来
2. 写低 word 为目标低 32
3. 再写高 word 为目标高 32
```

三步改一次值。丑，但对。

### 7.4 TEMU 为什么不修这个 hazard？

我们也能模仿 CLINT 加一个 "staging" 寄存器，软件必须两次写才 commit。但：

- 现有测试（test_timer_monotonic）只读不写 mtimecmp
- Stage 6a-4 的定时器中断 demo 只 arm 一次，永远不 re-arm
- 未来 Stage 6b 若跑真 Linux，那时候 Linux 自己知道先写高 word

所以 TEMU 当前**如实反映硬件行为，注释标注 hazard 给未来看**。这是 **"留 doc 不留 workaround"** 的典型——

> **核心洞察**：工程上有两种防错策略：**"让错误不可能发生"**（加状态机）和 **"让错误一发生立刻暴露"**（assert/panic）。再加一种**"行为忠实于硬件，错误在上层解决"**（留 doc）。TEMU 作为硬件模型，第三种最贴合它的定位——它本就该像硬件一样可以被用错。

### 7.5 双接口：为什么 timer 既是 MMIO 又是 C 函数？

`device.h:32` 里除了 `init_devices` 还有：

```c
uint64_t timer_mtime(void);
uint64_t timer_mtimecmp(void);
```

guest 通过 MMIO 访问 timer——`lw t0, 0(0xa0000048)`。那为什么还要暴露 C 函数？

**因为 Stage 6a 的 `cpu_exec` 每执行完一条指令都要判断**：

```c
if (timer_mtime() >= timer_mtimecmp()) {
    cpu.csr.mip |= MIP_MTIP;
}
```

如果走 MMIO 路径（`paddr_read(0xa0000048, 4)`），每条指令都：

1. 触发 `mmio_access`
2. 触发 `paddr_touched_mmio = true`
3. 导致 `difftest_step` 把每条指令都当"碰过 MMIO"处理
4. Snapshot 整个寄存器文件——**hot path 被完全废掉**

所以我们开两条路：

| 访问源 | 路径 | 副作用 |
|--------|------|--------|
| Guest 程序 | MMIO: `lw` → `paddr_read` → `mmio_access` → `timer_cb` | 触发 `paddr_touched_mmio`，走 difftest snapshot |
| Host `cpu_exec` | 直接 `timer_mtime()` C 函数 | 无副作用 |

> **核心洞察**：**同一个底层状态，对不同请求源暴露不同接口**。Guest 看到的是 MMIO 时序、有副作用、慢；Host 内部看到的是 C 函数、零副作用、快。这不是洁癖违反，是**工程必须**。
>
> 硬件里有类似的东西：CPU 有两条路径访问物理寄存器——"正常流水线" 和 "JTAG 调试端口"。两条路径读同一位数据，但时延、可见性、副作用都不同。TEMU 的"MMIO + C 函数"是它的软件版本。

### 7.6 理论视角：Deterministic simulation

§7.2 的 wall clock vs instruction count 表里埋了个大坑：**"不可复现"**。这一节展开为什么这是**分水岭级**的工程问题。

**问题动机：复现一个 bug**

假设你写了个 OS 调度器跑在 TEMU 上，10 万条指令里某一次遇到 `scheduler double-fault`。你想修这个 bug——第一步是**再次重现它**。于是你重跑同一个镜像：

- 如果 TEMU 是**确定性**的 → 每次都在第 K 条指令崩，可以单步上去看状态
- 如果 TEMU 不是确定性的 → **每次崩的位置不同**，可能跑 100 次才中一次，调试成本爆炸

不可复现的 debug 不是 debug，是考古。

**什么让模拟器"不确定"？**

几个典型源头：

| 源头 | 举例 | TEMU 现状 |
|--|--|--|
| Wall clock | `gettimeofday()` | ✅ timer_mtime 有 |
| 系统时间 | `time()`、`clock()` | 目前没用，未来建模 syscall 会有 |
| 真随机数 | `/dev/urandom`、RDRAND | 未建模 |
| 外部中断到达时机 | 键盘、网络、IPI | P6a 的 mtime 中断：和 wall clock 绑 |
| 线程调度 | 多核乱序 | TEMU 单核，不涉及 |
| 内存地址随机化 | ASLR、malloc 抖动 | pmem 是固定数组，不涉及 |
| 外部 I/O 内容 | `read(stdin)` 返回什么 | `serial_cb` 读返回 0，已确定 |

**TEMU 目前只有一个非确定性源头：mtime 基于 gettimeofday**。这意味着**只要 guest 程序不读 mtime**，TEMU 就是完全确定的——同一镜像跑 N 次，每一步 GPR / PC 都一模一样。

**关键洞察：Deterministic 是一个等级谱**

| 等级 | 承诺 | 工具代表 | 代价 |
|--|--|--|--|
| Bit-exact deterministic | 每条指令后状态完全一致 | Spike（无设备时）、TEMU（不读 mtime 时）| 最便宜 |
| Event-deterministic replay | 记录非确定事件 + 指令数索引，回放时注入 | ReVirt、VMware R/R | 需要日志，存储开销 |
| Sequentially consistent | 多核内操作有全序 | Java memory model | 性能损失 10–50% |
| Relaxed + replay hooks | 正常跑非确定性，特殊 log | rr（Mozilla）| 接入成本中等 |

TEMU 的教学定位刚好卡在**等级 1 + 小开洞**：默认 bit-exact，timer 是刻意留的开洞。这符合"先教清楚、再教工程"原则——学生先理解"为什么模拟器可以做到确定性"，再理解"为什么有时候要故意放弃一点"。

**工程里的 deterministic simulation（DST）**

这个词 2010 年代被**分布式系统社区**重新发明并火起来：

- **FoundationDB（2012, Apple）**：整个存储引擎 + 网络层用确定性模拟，"一颗随机数种子决定整个宇宙"。把分布式 bug 的复现从"跑三周压力测试"压到"秒级重现"
- **TigerBeetle（2020–）**：复用 FoundationDB 思路，把 I/O、时钟、磁盘、网络全部模拟化，每个测试都是一颗种子
- **Antithesis（2023）**：商业化 DST 平台，给客户端 OS 之上全栈模拟

核心洞察是**把"生产代码"和"时钟 / IO / 随机"解耦**——底层注入的可以是"真操作系统"（生产）也可以是"模拟器"（测试），业务代码不改。

**TEMU 和 DST 的关系**：

- TEMU 是 "CPU 级" 的 deterministic simulation——把 CPU + pmem 做成"可重现的状态机"
- FoundationDB / TigerBeetle 是 "进程级"的——把整个 OS + 网络做成"可重现的状态机"
- **两者同构**：都是"把非确定性事件降维成种子 + 事件序列"，再在回放层按序注入

**⚠️ 常见误解**

> "Deterministic simulation 就是 replay"

❌ 反了。Replay 是 DST 的**一个用法**；DST 更根本的是**给你一个控制时间的能力**——你可以加速、减速、倒流、分叉模拟，而不只是重放一次。

> "TEMU 这种小玩具不需要 DST"

反思：即便 TEMU 的规模只有几万条指令，一个跑错的 pc_stuck bug 如果不可复现，你也不敢动它。**所有状态机型软件都从"可复现"里收益**——只是收益-代价比随规模变化。

**对 TEMU 的自省**

- P5 为了"看起来活"把 wall-clock 引入了——典型的 **pragmatic 决定**
- 代价：一旦 guest 程序读 mtime，TEMU 就失去 bit-exact 复现
- 补偿：P4 的 difftest snapshot 机制让这件事在**对拍场景**里不破坏正确性证明
- 练习 Easy 2（timer 单调性保护）和 Medium 2（模拟器级 `--deterministic-timer` flag）是把这个开洞"按需关闭"的入口

**连接**：这个取舍和 P4 §9.2 的 "lockstep 三等级" 是**同一个哲学**——工程上的精度是"连续光谱"，不是 0/1 开关。写 TEMU 让你理解两头：什么时候要精度、什么时候可以放一点换速度。这是计算机科学和软件工程的**核心 trade-off 直觉**。

---

## 8. difftest × device：P4 伏笔终于兑现

P4 §6 讲过 `paddr_touched_mmio` 在差分测试里被检查。到 P5 它终于有"真东西"可指向：

```
         guest: sw t0, 0(a0)       (a0 = SERIAL_BASE)
                    │
                    ▼
         paddr_write(0xa00003f8, 4, 0x68)
                    │
                    ▼
         mmio_access → serial_cb → putchar('h'), fflush
                    │
                    ▼
         paddr_touched_mmio = true
                    │
       (this instruction is done, cpu_exec returns)
                    │
                    ▼
         difftest_step
             if (paddr_touched_mmio) {
                 paddr_touched_mmio = false;
                 ref_regcpy_from_main();      ← 不让 ref 再跑一次
                 return;                       ← 不比对
             }
             ref_exec_one();
             compare(ref_cpu, cpu);            ← 否则走正常 lockstep
```

### 8.1 为什么不让 ref 再跑一次？

**因为 ref 再跑一次 = 再打印一个 'h'**。你的 stdout 会看到 `hh`，程序输出"hhehllllo\n"——副作用翻倍。

更一般地：

| 副作用类型 | 在 ref 里重放会怎样 |
|-----------|---------------------|
| `putchar` | 字符出现两次 |
| 读 `mtime` | 两次返回值不同（gettimeofday 变了） |
| 未来的 `write` syscall | 文件被写两次 |
| 网络包发送 | 包发两次 |

**差分测试的前提是两套实现都是纯函数**。MMIO 一旦把副作用泄到宿主，这个前提就破了。解决：把这条指令**整个从比对中豁免**，直接复制状态。

### 8.2 豁免语义的代价

这样 MMIO 相关指令就失去了差分测试保护。如果你写错了 serial_cb（比如把 `*data & 0xff` 写成 `*data & 0xf`），difftest 不会告诉你——输出 `x%` 乱码你得靠肉眼发现。

> **核心洞察**：差分测试最优雅的边界不是"怎么比"而是"不比什么"。**覆盖率的代价 = 你必须准确划出那些"不比"的范围**。划大了漏 bug，划小了真值比对里全是噪声。
>
> TEMU 的划法是"走过 MMIO 的整条指令"。更精细的划法（比如只豁免命中 MMIO 那个字节对应的 ref 状态）理论上更好，但工程代价远超教学收益。

---

## 9. `init_devices()`：main 里的一行

`src/device/mmio.c:53`：

```c
void init_devices(void) {
    init_serial();
    init_timer();
}
```

`main.c:135` 调用它：

```c
cpu_init();
init_devices();       /* ← 必须在 cpu_init 之后、cpu_exec 之前 */
```

顺序：

1. `cpu_init` 把 `cpu.gpr` 清零，`cpu.pc = RESET_VECTOR`
2. `init_devices` 注册所有 MMIO map，记录 `boot_time_us = now_us()`
3. `load_img` 把镜像拷到 pmem
4. `cpu_exec` 开跑

### 为什么 `init_devices` 要 idempotent？

看 mmio_add_map 的 assert：

```c
Assert(nr_map < MAX_MMIO, "too many MMIO mappings (%d)", MAX_MMIO);
```

重复注册同名 map 会**再占一个槽、然后因重叠触发 panic**。所以 `init_devices` 假设全程只调一次——这是对调用方的隐式要求。未来若要支持 "reset 后重新 init"，得先加 `mmio_clear_all()`。目前没这需求，YAGNI。

---

## 10. 陷阱清单

### 10.1 绕过 `paddr_read/write` 直接读 pmem 数组

`paddr_read/write` 做三件事：合法性检查、MMIO 分发、`paddr_touched_mmio` 置位。ISA 执行层直接操作 `pmem[addr]` 会**跳过**MMIO 分派——于是 `sw` 到 `0xa00003f8` 就变成"写到 pmem 数组末尾"，serial 永远收不到字符。铁律：**isa 层只用 `paddr_*`**，pmem 数组只属于 `pmem.c` 内部。

### 10.2 `cpu_exec` hot path 走 MMIO

对 host 内部要频繁读的状态（比如 `timer_mtime`），必须用**直 C 函数**而非 MMIO。每条指令走一次 `paddr_read(0xa0000048, 4)` 会让 `paddr_touched_mmio = true` 每步触发、difftest snapshot 每条指令——整个对拍机制被废掉。规则见 §7.5 的双接口设计。

### 10.3 callback 里 `printf` 调试

serial 通过 `putchar + fflush(stdout)` 输出，**debug 打印也走 stdout 就会和 guest 输出交叉**：测试断言 `"hello"` 结果收到 `"[timer.c:50] reading mtimehello"`，程序测试全线崩。用 `Log()`/`fprintf(stderr, ...)` 写 stderr。

### 10.4 `paddr_touched_mmio` 的清零责任模糊

当前设计：`difftest_step` 开头清零（P4 的约定）。如果你写了自己的非 difftest 消费者（trace、profiler 等）并也读这个 flag，就会和 difftest 抢——要么 difftest 把消费者的读"吃掉"，要么消费者把 difftest 的读"吃掉"。要么改造成"`cpu_exec` 循环末尾清零"（全局统一约定），要么每个消费者自带读-清对。

### 10.5 MMIO 区域和 pmem 重叠

当前靠**约定不重叠**：pmem 在 `0x80000000..0x88000000`，MMIO 在 `0xa0000000+`。如果未来扩大 pmem 到 256 MB（`0x80000000..0x90000000`）就擦到 MMIO 区了——需要**同时**在 `mmio_add_map` 里加 assert：新 map 和 pmem 范围不交。现在没加是因为硬编码的地图不变。

### 10.6 `fflush(stdout)` 不能少

stdout 在非 tty 环境（被 pipe 到文件）默认**全缓冲**，4KB 不满不出来——5 字节 "hello" 可能永远不可见。serial_cb 每写一字节都要 fflush。性能代价可以接受因为 guest 本来写串口就不频繁，而且 fflush 是主要的 **bug 可观测性机制**——不要省。

### 10.7 mtimecmp 64-bit 半写 hazard

`sw mtimecmp_lo` 和 `sw mtimecmp_hi` 之间如果 mtime 正好跨过新低位值，会瞬间误触发中断（§7.3）。TEMU 不修这个坑（§7.4 解释了教学取舍）。如果你把 timer 当真的看，正确姿势是先写 `mtimecmp_hi = 0xFFFFFFFF`（拉到不可能触发）、再写 lo、最后回写 hi——这是 Linux 源码里的顺序。

### 10.8 MMIO 地址 "0x3f8" 凭空出现

§6.6 解释了——来自 1981 年 IBM PC 的 COM1 port 号。不是 TEMU 拍脑袋选的，是继承 Spike / QEMU / xv6 的上游约定。改成别的地址不会错，但所有教学资料都得同步改。

### 10.9 `mmio_add_map` 顺序依赖

当前 `init_devices()` 先 `init_serial` 后 `init_timer`，重叠检测只针对"之前注册过的 map"。如果你加第三个设备，记得让注册**在 init_devices 里**、而不是在 callback 被第一次触发时懒注册——懒注册让重叠检测失去锚点。

---

## 11. 练习

### Easy 1 · 给 serial 加一个 ready 状态位

让 `off=0` 继续是数据寄存器，`off=4` 变成 "TX ready" bit：

- 读 `off=4` 永远返回 1（我们的模型瞬间完成 TX，永远 ready）
- 写 `off=4` 被忽略（或 panic）

跑 `prog.c` 里加一个轮询 ready 再写的程序。**学到**：真实 UART 的 "status + data" 寄存器簇模型。

### Easy 2 · timer 的单调性保护

NTP 同步会把宿主系统时间往回调（极罕见，但有）。此时 `gettimeofday` 可能返回比上次更小的值，`timer_mtime` 就会**非单调递减**。加一个 max 保护：

```c
static uint64_t last_reported = 0;
uint64_t t = now_us() - boot_time_us;
if (t < last_reported) t = last_reported;
last_reported = t;
return t;
```

**学到**：wall clock 建模的陷阱；guest 程序依赖 "时间只会往前走"，你必须替它守住。

### Medium · 键盘输入（UART RX）

让 `serial_cb` 的 read 路径从宿主 stdin 拿字符。挑战：

1. **非阻塞读**：`getchar` 阻塞会冻结 guest；查 `select` / `fcntl O_NONBLOCK`
2. **EOF 语义**：stdin 关闭时返回 `-1`，guest 看到什么？常见做法是返回 0 + LSR bit 0 = 0（"no data"）
3. **terminal raw mode**：正常 tty 会等回车才把字符给进程，你得 `tcsetattr` 关掉 canonical mode

写一个 guest 程序 `echo 退出条件 'q'`：轮询 RX，读到 `q` 就 `ebreak`。

**学到**：从 "只有输出" 到 "全双工" 的难度跳跃——I/O 一半以上的复杂度在输入侧。

### Hard · VGA framebuffer

map `[0xa100_0000, 0xa100_0000 + 160*120)` 当成 160×120 字符缓冲区（每字节一个 ASCII 字符）。修改 `cpu_exec`：每执行 N 条指令，把整个 framebuffer 用 ANSI 转义码刷到终端。

挑战：

1. **帧率节流**：每条指令都刷新会把终端打爆。60 Hz 刷一次合适
2. **设备寄存器 vs 帧缓冲**：framebuffer 是**没有 callback 的 MMIO**——就是挂在那里的一段内存，硬件异步读出来。你要么给它一个真 pmem 区域、让 CPU 直接访问字节，要么在 mmio_cb 里维护一个 shadow buffer
3. **dirty tracking**：上一帧和这一帧只差几个字节时，没必要全屏重绘

**学到**：从 CPU 角度理解 "显卡是一块被 CPU 和显示器共同读写的内存"。这是现代 GPU 和共享内存硬件加速的思想原型。

---

## 12. 本章小结

**你应该能做到**：

- [ ] 解释为什么 RISC-V 一条 I/O 指令都没有还能做 I/O
- [ ] 画出 `paddr_read` 分派流程图（pmem → MMIO → panic）
- [ ] 读 `src/device/mmio.c` 写第二个设备（debug 寄存器、假硬币随机源……）
- [ ] 解释 serial 为什么必须 `fflush(stdout)`
- [ ] 解释 timer 为什么同时暴露 MMIO 和 C 函数接口
- [ ] 跑通 `make test-prog` 看到 `serial hello` 和 `timer monotonic` 两条绿

**你应该能解释**：

- [ ] Port I/O 和 MMIO 的 trade-off
- [ ] `paddr_touched_mmio` 这个 flag 如何串起 P2 / P4 / P5 三章
- [ ] Wall clock 和 instruction count 对"时间"建模的哲学差异
- [ ] mtimecmp 64-bit 写入 hazard 及 Linux 的规避手法
- [ ] Difftest + MMIO 为什么必须走 snapshot 而不是 lockstep
- [ ] 同一底层状态为什么对 guest 和 host 暴露不同接口

---

## 13. 延伸阅读

- **SiFive Interrupt Cookbook（v1.2，2020）** —— CLINT/PLIC 官方规范，`mtime`/`mtimecmp` 的定义来自这里
- **Linux `drivers/clocksource/timer-riscv.c`** —— 真实 kernel 怎么操作 mtimecmp，三步写入的活化石
- **QEMU `hw/char/serial.c`** —— 900 行完整建模 16550，对照 TEMU 30 行理解"玩具"和"工业"的鸿沟
- **Patterson & Hennessy, RISC-V Edition, Ch. 5.2** —— I/O 范式对比的教科书章节
- **Intel® 64 IA-32 Software Developer Manual Vol. 1 § 16** —— port I/O 的血统，x86 为什么保留它的历史原因
- **Bunnie Huang, *Hacking the Xbox*, Ch. 2** —— 从逆向角度看真硬件的 MMIO 地图

---

## 与后续章节的连接

| 下一章做什么 | 本章埋下的伏笔 |
|-------------|----------------|
| P6a Trap 机制 | `ebreak` / `ecall` 走 trap 后，CSR（mepc/mcause/mtvec）开始介入 |
| P6a 中断 delivery | `timer_mtime() >= timer_mtimecmp()` 这行代码变成 `mip.MTIP = 1` 的触发条件 |
| P6a CSR 差分 | `CSR_CMP` 从 P4 的"预留"变"激活"——MMIO flag 和 CSR 豁免共用同一个 snapshot 机制 |
| P6b 虚拟内存 | 设备地址段必须**绕过**页表（不能被 mmap 到用户空间），这是"物理地址"概念第一次和"虚拟地址"分野 |
| 全书剩余 | **P5 之后 TEMU 是一台能说话的计算机**——输出 `hello world`、能读时钟、能被差分测试守住。是时候让它学会**被打断**了 |

P5 给 TEMU 装上了嘴巴和钟表。P6a 要给它装上神经系统——让它能**被外部事件打断**，而不是一味地顺流而下。

**下一站：P6a——异常与中断，让 CPU 第一次学会"放下手头的事"。**
