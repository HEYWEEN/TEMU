# Stage 5 — MMIO 设备

> **状态**：📝 设计稿
> **验收命令**：程序能向串口 MMIO 写字节并在终端看到输出

---

## 1. Problem motivation

> 纯 CPU + 内存的程序只能**改自己的内存**，不能和外界说话。

想让模拟器打印 `"hello\n"`，CPU 必须能触发"外部副作用"。真实硬件靠**设备**做到这点：键盘、串口、屏幕、定时器、磁盘……它们挂在总线上，CPU 通过某种方式和它们交互。

**两种交互方式**：

| 方式 | 代表 ISA | 描述 |
|------|---------|------|
| **Port I/O** | x86 `in/out` | 专门的 I/O 指令 + 独立的 I/O 地址空间 |
| **MMIO** | RISC-V / ARM | 把设备寄存器**映射到内存地址**上，普通 load/store 就能访问 |

RISC-V 只用 MMIO——语言统一、ISA 设计简洁。

---

## 2. Naive solution & why not

> 给 CPU 加几个专门的 `syscall` 指令。

问题：

- **破坏 ISA 规范一致性**：程序员写的是标准 RV32I 代码，不应该为我们模拟器加语义
- **测试不可迁移**：真实硬件没有你这条指令，程序就不能跨平台
- **和 `gcc` 生态不兼容**：libc / newlib 期望 MMIO

所以必须走 MMIO 这条"业界标准"路。

---

## 3. Key idea

> **把一段物理地址划为"设备区"；`paddr_read/write` 路由到设备回调，不再读写 pmem 数组。**

对 CPU 来说，它还是在执行 `sw` / `lw`。对我们模拟器来说，这些访问被拦截后调了 `putchar` 或更新定时器。

---

## 4. Mechanism

### 4.1 路由表

```c
typedef struct {
    paddr_t lo, hi;               // 区间 [lo, hi)
    void (*callback)(paddr_t off, int len, bool is_write, word_t *data);
    const char *name;
} MMIO_Map;

static MMIO_Map mmio_map[MAX_MMIO];
static int nr_mmio = 0;

void mmio_add_map(paddr_t lo, paddr_t hi,
                  void (*cb)(paddr_t, int, bool, word_t *),
                  const char *name);
```

### 4.2 `paddr_read/write` 路由改造

```c
word_t paddr_read(paddr_t addr, int len) {
    if (in_pmem(addr)) return pmem_read(addr, len);
    for (int i = 0; i < nr_mmio; i++) {
        if (addr >= mmio_map[i].lo && addr < mmio_map[i].hi) {
            word_t data = 0;
            mmio_map[i].callback(addr - mmio_map[i].lo, len, false, &data);
            return data;
        }
    }
    panic("paddr_read: out of bound " FMT_PADDR, addr);
}
```

写入同理。

### 4.3 串口设备（最小款）

选一个保留地址，比如 `0xa00003f8`（仿 UART 起始）。**只有一个寄存器**：写入 = 输出到 stdout。

```c
#define SERIAL_BASE 0xa00003f8u
#define SERIAL_SIZE 8

static void serial_cb(paddr_t off, int len, bool is_write, word_t *data) {
    (void)off; (void)len;
    if (is_write) {
        putchar((*data) & 0xff);
        fflush(stdout);
    } else {
        *data = 0;    // 读串口 = 0（简化，无输入）
    }
}

void init_serial(void) {
    mmio_add_map(SERIAL_BASE, SERIAL_BASE + SERIAL_SIZE, serial_cb, "serial");
}
```

### 4.4 定时器设备（只读 64-bit）

```c
#define TIMER_BASE 0xa0000048u
#define TIMER_SIZE 8

static uint64_t boot_time_us;

static void timer_cb(paddr_t off, int len, bool is_write, word_t *data) {
    Assert(!is_write, "timer is read-only");
    uint64_t now = get_time_us() - boot_time_us;
    if (off == 0)      *data = (word_t)(now & 0xffffffff);   // 低 32 位
    else if (off == 4) *data = (word_t)(now >> 32);           // 高 32 位
    else panic("timer: bad offset %d", off);
}
```

### 4.5 程序侧的使用

C 代码写串口：

```c
volatile char *serial = (char *)0xa00003f8;

void putc(char c) { *serial = c; }
void puts(const char *s) { while (*s) putc(*s++); }
int main(void) { puts("hello\n"); return 0; }
```

编译：

```bash
riscv64-unknown-elf-gcc -march=rv32i -mabi=ilp32 \
    -nostdlib -nostartfiles -T link.ld \
    tests/programs/hello.c crt0.S -o hello.elf
riscv64-unknown-elf-objcopy -O binary hello.elf hello.bin
./build/temu -b hello.bin
# hello
```

---

## 5. Concrete example —— 一次 `sw t0, 0(a0)` 到串口

```
CPU 状态：a0 = 0xa00003f8, t0 = 'h' (0x68)
执行: sw t0, 0(a0)
  └→ paddr_write(0xa00003f8, 4, 0x68)
     └→ 不在 pmem 区（0x80000000 + 128MB = 0x88000000，串口在 0xa0...）
     └→ 查 mmio_map → 命中 serial 条目
     └→ serial_cb(off=0, len=4, is_write=true, data=0x68)
        └→ putchar(0x68 & 0xff) → 屏幕上打出 'h'
```

---

## 6. Acceptance

| 检查项 | 方式 |
|--------|------|
| `hello.bin` 跑通 | 屏幕输出 `hello\n` |
| 定时器递增 | 连续读两次 TIMER_BASE，第二次应 >= 第一次 |
| 越界访问 panic | `lw` 一个未注册的地址应 panic |
| Difftest 兼容 | 设备区的地址在 difftest 里应跳过比对 |

---

## 7. Limitations / pitfalls

### 7.1 `volatile` 关键字

C 程序写设备指针**必须用 `volatile`**，否则编译器会觉得"反正 `*serial = 'h'` 后没人读 serial"，把写操作优化掉。

### 7.2 设备寄存器的宽度

真实 UART 每个寄存器是 8 bit，但我们用 `sw`（4 字节）访问。两种设计：

- **严格仿真**：用 `sb` 写单字节，`sw` 访问 panic
- **宽容仿真**：`sw` 就只取低 8 位写设备

Stage 5 用**宽容**款，简单优先。

### 7.3 定时器的时间基准

`get_time_us()` 用宿主机 `gettimeofday` 或 `clock_gettime(CLOCK_MONOTONIC)`。这让"模拟器里的时间"和真实墙钟一致——好处是能测睡眠；代价是**执行慢的时候时间流逝快**（CPU 跑得慢但定时器按真实时间走）。

对这个项目没影响；真要严格就让定时器按"已执行指令数 / 假设频率"来算。

### 7.4 中断还没做

真实串口接收数据时会**中断** CPU。stage 5 **不做中断**，要求 stage 6 一起上异常机制才行。所以：

- 串口暂时只能**输出**
- 定时器暂时只能**轮询**

---

## 8. 与后续 stage 的连接

| Stage 5 留下的东西 | 谁会用它 |
|-------------------|----------|
| `mmio_add_map` 框架 | Stage 6 加中断控制器也走这条路 |
| 串口 | Stage 6 跑 OS 时给用户态程序用 |
| 定时器 | Stage 6 OS 调度器驱动时钟中断 |

---

## 9. 与 OS 课的连接

- **「设备即内存」**：这就是用户 OS 笔记里「地址空间包括设备」的具体实现
- **驱动的本质**：一段 C 代码 + 几个设备寄存器地址
- **为什么需要 `volatile`**：在模拟器里你能直接看到，编译器优化 = bug，设备寄存器不是普通内存
