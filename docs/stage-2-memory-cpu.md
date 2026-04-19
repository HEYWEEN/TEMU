# Stage 2 — 物理内存 + 寄存器 + PC

> **状态**：📝 设计稿（待 stage 1 完成后再细化）
> **验收命令**：`make && ./build/temu image.bin`，`info r` 正确打印 32 个寄存器 + PC

---

## 1. Problem motivation

> 「**进程 = CPU 状态 + 可访问内存**」——用户 OS 笔记的核心心智模型。

本 stage 的任务就是把这句话在 C 里实例化：定义一个能被 CPU 取指、能被 load/store 访问的**物理内存**，加上一套**寄存器 + PC**。

做完这 stage 之后，内存和寄存器就是 C 里两个普普通通的全局数据——没有魔法，可以 `printf` 它们、`memcpy` 它们。

---

## 2. Naive solution & why not

> `malloc(128 * 1024 * 1024)` 拿块堆内存当 pmem。

问题不大，但有三个理由用**静态数组**：

1. **启动即就位**：不存在「内存还没分配」的中间态，不需要写「初始化失败」分支
2. **地址稳定**：`pmem` 的起始地址编译期确定，调试时 `p &pmem[0]` 输出简单
3. **工具链限制**：不引入 `malloc` 失败的错误处理路径，代码干净

```c
// 直接在全局作用域：
static uint8_t pmem[PMEM_SIZE];
```

> **取舍标签**：pragmatic choice。128MB 静态数组在现代机器上零成本。

---

## 3. Key ideas

### 3.1 真实 RISC-V 的物理地址布局

RISC-V 惯例：

```
0x00000000 ─┐
            │   外设 / ROM / 低地址保留
0x80000000 ─┤ ← DRAM 起点（我们的 pmem 映射在这）
            │
            │   ← pmem[0 .. PMEM_SIZE-1] 就是这块
0x88000000 ─┘ ← 128MB DRAM 结束
```

**CONFIG**：

```c
#define PMEM_BASE  0x80000000u
#define PMEM_SIZE  (128 * 1024 * 1024)
#define RESET_VECTOR PMEM_BASE
```

PC 在模拟器启动时初始化为 `RESET_VECTOR`，指令镜像从这里开始被 `memcpy` 进 pmem。

### 3.2 地址翻译（stage 2 的极简版）

Stage 2 还没有 MMU，所以「虚拟地址 = 物理地址」。但接口要**预留分层**：

```c
typedef uint32_t vaddr_t;   // 虚拟地址
typedef uint32_t paddr_t;   // 物理地址

// Stage 2: 简单的 guest-to-host 翻译
static inline uint8_t *guest_to_host(paddr_t addr) {
    return pmem + (addr - PMEM_BASE);
}
```

**越界必须 panic**：

```c
static inline bool in_pmem(paddr_t addr) {
    return addr >= PMEM_BASE && addr < PMEM_BASE + PMEM_SIZE;
}
```

### 3.3 寄存器文件

```c
typedef struct {
    word_t gpr[32];
    vaddr_t pc;
} CPU_state;

extern CPU_state cpu;         // 全局唯一
```

**核心约束**：`x0` 硬连线为 0。**写 x0 必须被丢弃**。

实现方式有两种：

| 方式 | 做法 | 代价 |
|------|------|------|
| A. 写时判断 | `if (rd != 0) cpu.gpr[rd] = v;` | 每条写指令多一次分支 |
| B. 执行后清零 | 每条指令结束后 `cpu.gpr[0] = 0;` | 一次无条件写，无分支 |

**推荐 B**：简单正确，性能开销可忽略。

### 3.4 寄存器别名

RISC-V 的 ABI 给 32 个通用寄存器都起了名字：

| Index | Name | 用途 |
|-------|------|------|
| x0  | zero | 硬连线 0 |
| x1  | ra   | return address |
| x2  | sp   | stack pointer |
| x3  | gp   | global pointer |
| x4  | tp   | thread pointer |
| x5–x7  | t0–t2   | temporaries |
| x8  | s0 / fp | saved / frame pointer |
| x9  | s1   | saved |
| x10–x11 | a0–a1 | function args / return values |
| x12–x17 | a2–a7 | function args |
| x18–x27 | s2–s11 | saved |
| x28–x31 | t3–t6 | temporaries |

`reg_by_name` 要同时认 `x5` 和 `t0`。

---

## 4. Mechanism

### 4.1 接口（`include/memory.h`）

```c
// 读 len 字节（len ∈ {1, 2, 4}），返回 0 扩展到 32 位的结果
word_t paddr_read(paddr_t addr, int len);

// 写 len 字节
void   paddr_write(paddr_t addr, int len, word_t data);
```

**小端实现**（RISC-V 默认小端）：直接 `memcpy` 即可，因为宿主 macOS / Linux on x86-64 / arm64 也是小端。

```c
word_t paddr_read(paddr_t addr, int len) {
    assert(in_pmem(addr) && in_pmem(addr + len - 1));
    word_t ret = 0;
    memcpy(&ret, guest_to_host(addr), len);
    return ret;
}
```

### 4.2 镜像加载

```c
long load_img(const char *img_file) {
    FILE *fp = fopen(img_file, "rb");
    Assert(fp, "cannot open image '%s'", img_file);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    Assert((size_t)size <= PMEM_SIZE, "image too large: %ld", size);
    fseek(fp, 0, SEEK_SET);
    fread(guest_to_host(RESET_VECTOR), 1, size, fp);
    fclose(fp);
    return size;
}
```

### 4.3 CPU 初始化

```c
void cpu_init(void) {
    memset(&cpu, 0, sizeof cpu);
    cpu.pc = RESET_VECTOR;
}
```

### 4.4 `info r` 实现

```c
static int do_info(char *args) {
    if (strcmp(args, "r") == 0) {
        for (int i = 0; i < 32; i++) {
            printf("  %-4s (x%-2d) = 0x%08x  %d\n",
                   reg_name(i), i, cpu.gpr[i], cpu.gpr[i]);
        }
        printf("  pc        = 0x%08x\n", cpu.pc);
    } else if (strcmp(args, "w") == 0) {
        print_watchpoints();
    }
    return 0;
}
```

---

## 5. Concrete example

```text
$ make && ./build/temu tests/programs/zero.bin
(temu) info r
  zero (x0 ) = 0x00000000  0
  ra   (x1 ) = 0x00000000  0
  sp   (x2 ) = 0x00000000  0
  ...
  t6   (x31) = 0x00000000  0
  pc        = 0x80000000
(temu) x 4 $pc
0x80000000: 0x00000013 0x00100093 0x00208113 0x003100b3
```

最后一行是 `x 4 $pc`：从 PC 开始扫 4 个 4 字节字。

---

## 6. Acceptance

| 检查项 | 方式 |
|--------|------|
| `cpu_init` 后 32 个寄存器为 0 | `info r` |
| PC 正确初始化为 `RESET_VECTOR` | `info r` 最后一行 |
| 镜像能加载 | 读一个已知 1KB 的镜像，比较内存前 1KB 与文件字节 |
| 写 x0 无效 | 手工把 x0 赋值（通过后续 stage 3 的 ADDI），`info r` 依然为 0 |
| 越界访问 panic | `x 1 0x40000000` 应 panic（非 pmem 区） |

---

## 7. Limitations / pitfalls

### 7.1 对齐

RISC-V 对 `lw` / `sw` 要求 **自然对齐**（地址是 4 的倍数）。Stage 2 的 `paddr_read` 先**不检查**，到 stage 3 实现 load/store 时再 enforce——因为对齐检查是**指令层语义**，不是内存层语义。

### 7.2 字节序

本项目假定宿主是小端。如果未来要跑在大端机器（比如某些 POWER 上），`paddr_read/write` 要手动拼字节。现代 mac / Linux 都是小端，无需担心。

### 7.3 `word_t` 的大小

Stage 2 定义 `word_t = uint32_t`，这在 RV32I 下正确。未来若扩到 RV64I 需要统一改成 `uint64_t`——**别硬编码 `uint32_t`，全用 `word_t`**。

### 7.4 MMIO 冲突

Stage 5 会加 MMIO 设备——某些物理地址会路由到设备回调而不是 pmem。现在写 `paddr_read/write` 时预留这个钩子：

```c
word_t paddr_read(paddr_t addr, int len) {
    if (in_pmem(addr)) return pmem_read(addr, len);
    // Stage 5 添加：
    // if (in_mmio(addr)) return mmio_read(addr, len);
    panic("paddr_read: out of bound " FMT_PADDR, addr);
}
```

---

## 8. 与后续 stage 的连接

| Stage 2 留下的东西 | 谁会用它 |
|-------------------|----------|
| `CPU_state cpu` | Stage 3 的所有指令实现 |
| `paddr_read/write` | Stage 3 的 load/store 指令 + 取指 |
| `load_img` | Stage 3 之后的程序测试 |
| `RESET_VECTOR` | Stage 3 的 cpu_exec 取指起点 |

---

## 9. 与操作系统课的连接点

- **「进程即状态」**：`struct computer { CPU_state cpu; uint8_t pmem[...]; }` 正是最小进程的定义
- **地址空间**：目前只有一个地址空间（物理），stage 6 引入分页后会有虚拟地址空间
- **ELF 加载**：当前 `load_img` 只加载 raw binary；stage 3 之后可以扩展成解析 ELF，对应 OS 笔记里的 `execve`
