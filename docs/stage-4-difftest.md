# Stage 4 — Difftest 差分测试

> **状态**：📝 设计稿
> **验收命令**：跑一个 10k 条指令的基准程序，与 Spike 逐步比对零差异

---

## 1. Problem motivation

> Stage 3 你写完了 40 条指令。你**确定**每条都对吗？

不确定。RV32I 指令不多，但**立即数拼位、符号扩展、分支偏移、LB/LBU 的扩展规则**——每一个都是独立的、沉默的 bug 源头。

**肉眼复查 + 单元测试**能抓到大部分错误，但抓不住：
- 两个 bug 相互抵消（符号扩展错 + 比较方向错，单测通过）
- 边界条件（`x0` 被意外写、PC 加 4 时算错）
- 长程序里累积的微小误差

---

## 2. Naive solution & why not

> 写更多测试。

会发现**测试永远覆盖不完**——RV32I 的输入空间是 2^32，你最多测几千条。

正确思路：**找一个你信得过的参考实现，每一步对比状态**。

---

## 3. Key idea — Differential testing

> **每执行一条指令**，把我们的 CPU 状态（寄存器 + PC）和参考实现的状态**逐字节比较**。不一致就立即 panic，输出 diff。

这把"在几百万条指令中的某一处出现微小状态偏离"的问题，变成"在**第一处偏离**时就叫停"。定位成本从指数级降到 O(1)。

**参考实现**：Spike（`riscv-isa-sim`），官方维护，公认权威。

---

## 4. Mechanism

### 4.1 Spike 以动态库形式嵌入

Spike 本身是个独立模拟器，但它暴露了 SO 接口给第三方调用：

```c
// libspike.so 对外符号（简化）
void  spike_init(const char *img, int img_size);
void  spike_exec_one(void);
void  spike_regcpy(void *dest, bool to_ref);  // 双向拷贝寄存器
void  spike_memcpy(paddr_t addr, void *buf, size_t sz, bool to_ref);
```

> **工程现实**：实际 API 名字是 `difftest_*`，PA 讲义给出了精确签名。

### 4.2 我们这一侧的 glue

```c
#include <dlfcn.h>

typedef void (*ref_init_fn)(const char *img, int img_size);
typedef void (*ref_exec_fn)(void);
typedef void (*ref_regcpy_fn)(void *, bool);

static ref_init_fn   ref_init;
static ref_exec_fn   ref_exec_one;
static ref_regcpy_fn ref_regcpy;

void difftest_init(const char *so_path, const char *img, int img_size) {
    void *h = dlopen(so_path, RTLD_LAZY);
    Assert(h, "dlopen '%s' failed: %s", so_path, dlerror());
    ref_init     = dlsym(h, "difftest_init");
    ref_exec_one = dlsym(h, "difftest_exec_one");
    ref_regcpy   = dlsym(h, "difftest_regcpy");
    ref_init(img, img_size);

    // 把我们的 pmem 拷到参考侧，保证起点一致
    ref_memcpy(PMEM_BASE, guest_to_host(PMEM_BASE), img_size, true);
    ref_regcpy(&cpu, true);      // 初始寄存器对齐
}
```

### 4.3 执行循环嵌入

```c
void cpu_exec(uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        decode_exec_one();
        if (difftest_enabled) {
            ref_exec_one();               // 参考侧也跑一步
            difftest_check_regs();        // 对比
        }
    }
}
```

### 4.4 `difftest_check_regs`

```c
void difftest_check_regs(void) {
    CPU_state ref_cpu;
    ref_regcpy(&ref_cpu, false);          // 从参考侧拷回

    bool ok = true;
    for (int i = 0; i < 32; i++) {
        if (cpu.gpr[i] != ref_cpu.gpr[i]) {
            Log("gpr[%d] (%s): mine=0x%08x  ref=0x%08x",
                i, reg_name(i), cpu.gpr[i], ref_cpu.gpr[i]);
            ok = false;
        }
    }
    if (cpu.pc != ref_cpu.pc) {
        Log("pc: mine=0x%08x  ref=0x%08x", cpu.pc, ref_cpu.pc);
        ok = false;
    }
    if (!ok) {
        dump_itrace_buffer();             // 打最近 16 条指令
        panic("difftest failed at pc=0x%08x", cpu.pc);
    }
}
```

---

## 5. Concrete example —— 一个真实 bug 被 difftest 抓到

假设我们 `BGE` 写成了 `BGEU`（把 signed 比较误写成 unsigned）：

```text
==> after pc=0x80001024 (bge x5, x6, -8)
gpr[5] (t0): mine=0xffffffff  ref=0xffffffff     ← 相同
gpr[6] (t1): mine=0x00000001  ref=0x00000001     ← 相同
pc:          mine=0x8000101c  ref=0x80001028     ← **PC 不一致！**

Itrace (last 8):
  0x80001020: 000302b3  add  t0, t1, t0
  0x80001024: fe531ee3  bge  t0, t1, -8
  ...
```

`bge` 的语义：`t0 >= t1` 时跳。`0xffffffff` 作为 signed = -1，不 >= 1 → 不跳 → pc = 0x80001028。但我们实现成了 unsigned：`0xffffffff >= 1` → 跳 → pc = 0x8000101c。

**没有 difftest，这会在程序某处突然崩溃，追踪到根因要几小时。**

---

## 6. Acceptance

| 检查项 | 方式 |
|--------|------|
| 能 `dlopen` Spike SO | 启动时不报错 |
| 初始状态一致 | 拷完 pmem + regs 后 `difftest_check_regs` 通过 |
| 顺序指令 100 条 零差异 | 跑一个全是 ADDI 的镜像 |
| 完整程序 一致 | 跑 `fib.c`，CPU 归零后和参考对比，pc 与寄存器全等 |
| 故意引 bug 能被抓 | 把 SUB 写成 ADD，difftest 应在**第一条 SUB** 处停下 |

---

## 7. Limitations / pitfalls

### 7.1 `--diff` 启动选项

给 `temu` 加一个 `--diff <so_path>` 参数；默认关闭 difftest（因为拖慢 2-5x）。

### 7.2 何时**关闭**对比

有几类指令两边行为会"合法地"不一致，需要跳过比较：

- **计时器读取**：`rdcycle` 取决于执行进度
- **设备 MMIO**：参考实现可能没有我们这边的串口
- **CSR 状态**：某些 CSR 实现差异

stage 4 的 difftest 只对**GPR + PC**做严格比较；内存只在自检时抽查。

### 7.3 内存对比太贵

每条指令比对全部 128MB 内存不现实。策略：**只在 store 后**对比**被 store 的那几字节**。

```c
// 伪代码
if (was_store_instruction) {
    uint8_t ours[8], refs[8];
    memcpy(ours, guest_to_host(store_addr), store_len);
    ref_memcpy(store_addr, refs, store_len, false);
    if (memcmp(ours, refs, store_len) != 0) panic(...);
}
```

### 7.4 参考实现的初始化坑

Spike 启动时寄存器**不一定全 0**（有些 CSR 有默认值）。我们的 difftest 起点要先 `ref_regcpy(&cpu, true)` **强制让参考和我们一致**，再开始执行。

### 7.5 编译 / 获取 Spike

```bash
# macOS 上最省事的是用 brew
brew tap riscv-software-src/riscv
brew install spike

# 或源码编译
git clone https://github.com/riscv-software-src/riscv-isa-sim
cd riscv-isa-sim && mkdir build && cd build
../configure --prefix=$PWD/install
make -j && make install
```

Spike 默认**不是** SO 形式。想作为库嵌入需要额外的 shim——PA 讲义提供了 shim 源码，直接拿来用最省事。

> **备选方案**：如果 Spike 接入痛苦，可以写**双套自实现**互相比对——比如 stage 3 的解码用两套完全不同的风格（INSTPAT 宏 vs 手写 switch），各自跑、互相 diff。这是穷人版 difftest，一样有效。

---

## 8. 与后续 stage 的连接

| Stage 4 留下的东西 | 谁会用它 |
|-------------------|----------|
| `difftest_check_regs` | Stage 5 加 MMIO 后继续保活——设备相关地址跳过比对 |
| 对比基础设施 | Stage 6 加异常后，还要比 `mcause / mepc` 等 CSR |

---

## 9. 心法

> **Difftest 是可靠 ISA 实现的分水岭。**

Stage 3 是"能跑"，Stage 4 是"能信"。没有 stage 4 你永远不知道哪里错了——出 bug 时会怀疑编译器、怀疑镜像、怀疑测试 harness，最后发现是自己 20 天前写错的一个 funct3。

有了 difftest 后，bug 会在**发生的第一条指令**就被叫停，调试时间从"天"变成"分钟"。
