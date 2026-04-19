# 怎么"玩" TEMU

> 这份文档不是设计规范，是一个能动手的游乐场。你克隆下来、编译完之后，照着做一遍就知道这台模拟器能做什么、怎么戳它、怎么往里塞自己的东西。

下面每一节都可以**独立跑**，顺序只是建议。命令都能直接复制粘贴。

---

## 0. 先把它编出来

```bash
make              # 编译 temu 和所有测试用的 helper
make test         # 跑一遍全部测试（应该全绿）
```

你应该看到类似这样的输出：

```
expr tests:     56 passed, 0 failed ✓
pmem load test: OK
pmem OOB test:  OK
isa tests:      48 passed, 0 failed
program tests:   6 passed, 0 failed
```

如果有红的，别继续往下，先看哪里坏了。

---

## 1. 打开 REPL，先逛一圈

```bash
make run
```

你会进到一个 GDB 风格的提示符：

```
TEMU — TErminal Machine Emulator (RV32I)
pmem: 0x80000000 .. 0x88000000 (128 MB)
pc:   0x80000000
Type 'help' for commands, 'q' to quit.
(temu)
```

**几分钟小逛**——每条都敲一遍：

```
help                         # 看有哪些命令
p 1 + 2                      # p = "print"，求值一个表达式
p 0xff & 0xaa                # 位运算、十六进制都认
p (1 << 8) - 1               # 256 - 1 = 255
info r                       # 打印所有 32 个寄存器 + PC
p $pc                        # 读寄存器当值用
p $pc + 16
x 4 $pc                      # 从 PC 起往后扫 4 个 4 字节字（目前是 0）
w $pc                        # 在 PC 上设一个监视点
info w                       # 查看监视点
d 1                          # 删掉监视点 #1
q                            # 退出
```

**注意几个细节**：

- `$pc` 在启动后就是 `0x80000000`（pmem 起始地址，CPU 复位向量）
- 表达式求值器认 C 的全部常见算子：`+ - * / % << >> & | ^ ~ ! && || == != < > <= >=`
- 解引用用 `*`：`p *0x80000000` 读 PC 处的 4 个字节
- 不懂什么语法 → 直接敲，读它的报错

---

## 2. 跑一个真正的程序：Hello World

TEMU 有一个 8 字节的串口映射在 `0xa00003f8`——写这个地址就是输出到终端。

**用 Python 构造一个把 "hello" 吐到串口的裸机程序**：

```bash
python3 <<'EOF' > /tmp/hello.bin
import struct
def lui(rd, imm20):       return (imm20 & 0xfffff000) | (rd << 7) | 0x37
def addi(rd, rs1, imm):   return ((imm & 0xfff) << 20) | (rs1 << 15) | (rd << 7) | 0x13
def sb(rs2, rs1, imm):
    i = imm & 0xfff
    return ((i >> 5 & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) | ((i & 0x1f) << 7) | 0x23
ZERO, T0, A0 = 0, 5, 10
prog = [
    lui(A0, 0xa0000000),                # a0 = 0xa0000000
    addi(A0, A0, 0x3f8),                # a0 = 0xa00003f8  (serial)
]
for ch in "hello\n":
    prog += [addi(T0, ZERO, ord(ch)), sb(T0, A0, 0)]  # putchar
prog += [addi(A0, ZERO, 0), 0x00100073]                # return 0; ebreak
import sys
sys.stdout.buffer.write(b"".join(struct.pack("<I", i) for i in prog))
EOF

./build/temu -b /tmp/hello.bin
```

输出：

```
[src/memory/pmem.c:81 load_img] loaded 56 bytes from '/tmp/hello.bin' at 0x80000000
hello
HIT END  pc=0x80000038  halt_ret=0x00000000 (0)
```

**`hello` 真的是你的 CPU 跑出来的**——它从 0x80000000 开始取指令，一条条 decode 执行，最终那六次 `sb` 把 6 个字节塞到串口 MMIO 地址。

---

## 3. 用调试器观察 Hello World 逐条跑

同一个 hello.bin，这回**不要** `-b` 批量模式，让它进 REPL：

```bash
./build/temu /tmp/hello.bin
```

```
(temu) si                      # 执行 1 条
(temu) info r                  # 看寄存器：a0 应该是 0xa0000000（LUI 后）
(temu) si                      # 再一条（ADDI 把 a0 调整到 0xa00003f8）
(temu) p $a0                   # = 0xa00003f8
(temu) si 4                    # 再跑 4 条（完成第一个 'h' 输出）
  ← 屏幕会冒出 "h"
(temu) w $a0                   # 在 a0 上设监视点
(temu) c                       # 继续跑，等 a0 变化时会停
  ← 但 a0 不再变（剩下的循环只写串口），所以会一路跑到 ebreak
```

**试这些**：

- `si 100` 跑 100 步（本程序只有 14 条，会在 ebreak 停下）
- `p *$pc` 看当前指令编码
- `x 8 0x80000000` 打印程序开头的 8 个字（你能从机器码里认出 LUI / ADDI）
- `w $pc == 0x80000010` 在某个 PC 值上停

---

## 4. 看差分测试是怎么工作的

TEMU 带了一套**第二实现**（`src/difftest/difftest.c`，用完全不同的代码风格写了同一个 RV32I）。加 `-d` 就让每条指令都在两边各跑一次、对比状态。不一致立刻停。

```bash
./build/temu -b -d /tmp/hello.bin     # 照常运行，没差异
```

输出：

```
difftest: initialized — ref pc=0x80000000
hello
HIT END ...
```

（`hello` 只出现一次，说明 ref 不重复副作用——MMIO 访问这一步 difftest 会跳过比对并直接把主状态拷给 ref。）

**要看 divergence 实际长啥样？** 手动改 ref 造一个 bug：

```bash
# 把 ADDI 的 '+' 改成 '-'
sed -i.bak 's|G(rd) = a + uns_i;.*/\* ADDI|G(rd) = a - uns_i;  /* broken ADDI|' \
    src/difftest/difftest.c
make > /dev/null
./build/temu -b -d /tmp/hello.bin 2>&1 | head -10

# 还原
mv src/difftest/difftest.c.bak src/difftest/difftest.c
make > /dev/null
```

你会看到：

```
  gpr[10] (a0  ): mine=0xa0000000  ref=0x60000000
difftest: CPU state diverged from reference
  last instruction: 0x???????? lui     a0, 0xa0000
```

**精确到指令级**——哪条指令、哪个寄存器、两侧各是多少。

---

## 5. 手搓一个你自己的程序

最舒服的写法是借 `tests/isa/isa-encoder.h` 里的汇编宏。假设你想写**计算 1+2+3+...+N，N=10 时结果是 55**：

在 `tests/programs/prog.c` 里加一个 `static void test_my_sum(void)`，放进 `main` 的调用列表：

```c
static void test_my_sum(void) {
    RUN("my sum 1..10 = 55", 55,
        ADDI(T0, ZERO, 10),          /* N */
        ADDI(A0, ZERO, 0),           /* sum */
        ADD(A0, A0, T0),             /* loop: sum += N */
        ADDI(T0, T0, -1),
        BNE(T0, ZERO, -8));          /* back to ADD */
}
```

然后：

```bash
make test-prog
```

这种「在测试文件里加一个 case」的套路，是给 TEMU 写程序最轻量的方式。想看复杂一点的例子？翻 `tests/programs/prog.c`：

- `test_iterative_fib` —— 迭代版 fib(10)=55
- `test_recursive_fib` —— **递归版 + 栈** fib(6)=8（会真用到 sp / sw / lw / jalr ra）
- `test_array_sum` —— LW 在循环里读数组
- `test_serial_hello` —— MMIO 串口输出

每个 case 都是几十条 RISC-V 指令级的汇编宏调用，读起来和真 asm 差不多。

---

## 6. 跑一次模糊测试

表达式求值器有一个差分 fuzz，会生成随机表达式，用宿主 `cc` 编出真值，再喂给 TEMU 比对：

```bash
make test-expr-fuzz                # 默认 500 条
make test-expr-fuzz N=2000         # 压一压
```

输出：

```
fuzz: generated=500  verified=500  skipped=0
expr tests: 500 passed, 0 failed ✓
```

想看它真的能抓 bug？临时把 `src/monitor/expr.c` 里 `parse_add` 的 `+` 改成 `-`，重跑。然后撤回改动。

---

## 7. 想深入到底层 —— 按这个顺序读源码

每个 stage 对应一个文档，但源码本身读起来也不长。推荐顺序：

| 想看什么 | 去哪读 |
|---------|-------|
| 程序怎么进来、怎么退出 | `src/main.c` |
| 调试器 REPL 怎么分派命令 | `src/monitor/sdb.c` |
| 表达式求值器（lex + parse + eval） | `src/monitor/expr.c` |
| 监视点怎么实现 | `src/monitor/watchpoint.c` |
| 物理内存 + MMIO 路由 | `src/memory/pmem.c`, `src/device/mmio.c` |
| 串口 / 定时器 | `src/device/serial.c`, `src/device/timer.c` |
| CPU 主循环、停机状态、itrace | `src/cpu/cpu_exec.c` |
| 指令解码 + 执行（pattern-match 表）| `src/isa/riscv32/inst.c` |
| 立即数抽取、SEXT、BITS 宏 | `src/isa/riscv32/local-include/inst.h` |
| 寄存器文件 + ABI 别名 | `src/isa/riscv32/reg.c` |
| 反汇编器 | `src/isa/riscv32/disasm.c` |
| **第二套 ISA 实现**（差分对比用） | `src/difftest/difftest.c` |

想弄清**某个指令**（比如 JALR）具体怎么跑？grep 一下：

```bash
grep -n jalr src/isa/riscv32/inst.c          # 主实现
grep -n 'JALR\|0x67' src/difftest/difftest.c # 参考实现
```

两套不同风格的实现对比着读，很能帮你建立对 ISA 的理解。

---

## 8. 感觉想挑战一下？几个往里塞的方向

难度从小到大：

**🍼 简单**

- 给 REPL 加一个 `reset` 命令：重置 CPU 状态回到刚加载镜像的初始值
- 把反汇编也暴露成一个 REPL 命令：`x/i N EXPR` 从 EXPR 地址反汇编 N 条指令
- 写一个新的 MMIO 设备：随机数寄存器、键盘输入

**🏃 中等**

- 实现 RV32M 扩展（MUL / DIV）—— 加 8 条指令，改 2 个文件
- 改 timer 从"墙钟"变成"指令计数"——让仿真可复现
- 给 CPU 加**指令统计**：跑完程序告诉你每条指令各用了多少次

**🥷 硬**

- 跑真的 `riscv64-unknown-elf-gcc` 编出来的 C 程序（要写 crt0.S + linker script）
- 实现异常 / 陷入机制（对应 Stage 6）
- 接入 Spike 做真正独立的差分测试

---

## 9. 快速查询卡

```
# 编译
make                    all targets (temu + test binaries + tools)
make tools              只编 tools/（gen-expr 等）

# 测试
make test               expr + pmem + isa + prog
make test-diff          isa + prog 同时开差分测试
make test-expr-fuzz     随机 fuzz 表达式求值器
make test-<name>        单独跑某档

# 运行
./build/temu                    REPL 模式，无镜像
./build/temu IMG                REPL 模式，加载 IMG
./build/temu -b IMG             批量模式，run to halt
./build/temu -b -d IMG          批量 + 差分测试
./build/temu -l FILE IMG        把日志写到 FILE
./build/temu -t EXPRFILE        跑表达式测试，不进 CPU

# REPL 命令
help                    列命令
c                       continue 到停机 / watchpoint
si [N]                  单步 N 条（默认 1）
info r | info w         查寄存器 / 查监视点
x N EXPR                从 EXPR 起扫 N 个 4 字节字
p EXPR                  求值并打印
w EXPR                  新监视点
d N                     删监视点 #N
q                       退出
```

---

祝玩得开心。有什么问题、发现什么 bug、想加新功能——直接动手。这玩意儿拢共 2000 行 C，怎么折腾都不会塌。
