# Stage 1 — Monitor / 调试器 + 表达式求值器

> **状态**：⏳ 待实现
> **验收命令**：`make test-expr` 全绿

---

## 1. Problem motivation

Stage 2 之后 CPU 会开始"执行指令"。执行的意思是**不断改变一些全局状态**（寄存器、PC、内存）。如果我们没有办法观察和操作这些状态，一旦出 bug 就只能靠 `printf` 大海捞针。

我们需要一个**交互式调试器**，具备以下能力：

- **观察**：`info r` 看寄存器、`x N EXPR` 扫描内存
- **控制**：`c` 跑到结束、`si N` 单步 N 条
- **求值**：`p EXPR` 把「寄存器、常量、算术、内存解引用」混合的表达式算出结果
- **监视**：`w EXPR` 注册监视点，值变化时自动停

关键是：**CPU 还不存在，我们就要先把调试器壳写好**。这不是形式主义——调试器里的**表达式求值器**是 stage 3 验证指令正确性的核心工具。

> ⚠️ **常见误解**：调试器是"辅助功能"，可以等 CPU 能跑了再加。错。没有调试器你根本不知道你的 CPU 是不是在正确地执行——调试器不是验证的旁观者，是验证的本体。

---

## 2. Naive solution & why not

> 每条指令后 `printf` 所有寄存器和关心的内存单元。

问题：

- 一次性打印一屏信息，**关心的变量淹没在噪声里**
- 不能按需求值复杂表达式：比如「sp 向下 0x10 处的 4 字节等于预期值吗？」
- 不能**条件停机**：比如「当 `*0x80002000 == 0xdeadbeef` 时停下来看」

所以必须上 **REPL + 表达式求值器 + 监视点**。

---

## 3. Key ideas

### 3.1 命令参考 GDB

用户已经熟悉 GDB 的 `c / si / info r / x / p / watch / delete / q`。复用这套词汇，学习曲线为零。

### 3.2 表达式求值用**手写递归下降 parser**

- 不上 `flex` / `bison`：这两个工具本身学习成本比手写 parser 还高
- 递归下降**和语法规则一一对应**，每个非终结符 = 一个 C 函数
- 调试方便：栈轨迹直接就是推导路径

### 3.3 先 tokenize，后 parse

一次性把整串输入切成 token 数组，parse 阶段只看 token 序列。这比"parse 时边切边看"简单太多。

---

## 4. Mechanism

### 4.1 模块划分

```
src/monitor/
├── sdb.c            # REPL 主循环 + 命令分派（接 stage 0 的 cmd_table）
├── expr.c           # 表达式求值：tokenize + parse + eval
└── watchpoint.c     # 监视点链表 + 命中检测
```

`include/` 下新增：

```
include/
├── common.h         # 基本类型（word_t, paddr_t, vaddr_t, bool）、Log、panic、Assert
├── monitor.h        # sdb 对外接口（sdb_mainloop、expr、set_wp、free_wp）
└── debug.h          # Trace 开关（ITRACE / MTRACE / FTRACE）— 先放占位
```

### 4.2 命令表扩充

在 stage 0 的 `cmd_table` 里加这些：

| 命令 | 参数 | 作用 |
|------|------|------|
| `c` | 无 | continue——让 CPU 一直跑到结束或命中监视点。stage 1 下 CPU 不存在，这条命令先写成 stub |
| `si` | `[N]` | 单步 N 条，默认 1。stage 1 下同样 stub |
| `info` | `r` / `w` | 打印寄存器 / 监视点 |
| `x` | `N EXPR` | 从 `EXPR` 地址扫描 N 个 4 字节 |
| `p` | `EXPR` | 求值并打印（十进制 + 十六进制） |
| `w` | `EXPR` | 新建监视点 |
| `d` | `N` | 删除编号 N 的监视点 |
| `q` | 无 | 退出（stage 0 已有） |

Stage 1 里 `c` / `si` / `info r` 都是 stub（CPU 还没实现），但要**写好接口**，stage 2/3 只需填函数体。

### 4.3 表达式文法（EBNF）

```
expr    = or_expr ;
or_expr = and_expr { "||" and_expr } ;
and_expr= eq_expr  { "&&" eq_expr } ;
eq_expr = rel_expr { ("==" | "!=") rel_expr } ;
rel_expr= bor_expr { ("<" | ">" | "<=" | ">=") bor_expr } ;
bor_expr= xor_expr { "|" xor_expr } ;
xor_expr= band_expr{ "^" band_expr } ;
band_expr=shift_expr{ "&" shift_expr } ;
shift_expr= add_expr { ("<<" | ">>") add_expr } ;   // 可选，RV32I 足够
add_expr= mul_expr { ("+" | "-") mul_expr } ;
mul_expr= unary    { ("*" | "/" | "%") unary } ;
unary   = ("-" | "!" | "~" | "*") unary             // 一元
        | primary ;
primary = NUMBER
        | HEX_NUMBER
        | REGISTER                                   // $pc, $x0..$x31, $ra, ...
        | "(" expr ")" ;
```

**优先级从低到高**：`|| < && < ==/!= < </><=/>= < | < ^ < & < <</>> < +/- < *//% < unary < primary`

这和 C 的优先级一致，避免用户出错。

### 4.4 Tokenize

```c
typedef enum {
    TK_NOTYPE = 256, TK_NUM, TK_HEXNUM, TK_REG,
    TK_EQ, TK_NEQ, TK_LE, TK_GE, TK_AND, TK_OR,
    TK_DEREF,                     // 后 fix 阶段从 '*' 改过来
    // 单字符 token 直接用 char 本身：'+' '-' '*' '/' '(' ')' ...
} token_type_t;

typedef struct {
    int  type;
    char str[32];
} Token;

static Token tokens[MAX_TOKENS];
static int   nr_token;
```

**策略**：按优先级排列一张规则表 `{ regex, token_type }`，从前往后匹配。正则用 `<regex.h>`（POSIX）。

**关键顺序**（顺序错了就会出奇怪 bug）：
1. `0x[0-9a-fA-F]+` → `TK_HEXNUM`（必须在 `NUM` **之前**，否则 `0x10` 被切成 `0` + `x` + `10`）
2. `[0-9]+` → `TK_NUM`
3. `\$[a-zA-Z0-9]+` → `TK_REG`
4. `==` / `!=` / `<=` / `>=` / `&&` / `||` → 两字符 token（必须在单字符 `=` `<` `>` `&` `|` **之前**）
5. 单字符 `+ - * / ( ) & | ^ ~ ! < > %` → 直接用 ASCII 值

### 4.5 一元 `*` / `-` 的消歧

歧义来源：`*` 既是乘法也是解引用，`-` 既是减法也是负号。

规则（业界常用 trick，PA 讲义里也这么写）：**如果 `*` 或 `-` 的前一个 token 不是「操作数或右括号」，它就是一元的**。

```c
// tokenize 之后扫一遍：
for (int i = 0; i < nr_token; i++) {
    if (tokens[i].type == '*' &&
        (i == 0 || !is_operand(tokens[i-1].type))) {
        tokens[i].type = TK_DEREF;
    }
    // 同理处理一元 '-' 和 '!'
}
```

`is_operand(t)`：`t` 是 `TK_NUM` / `TK_HEXNUM` / `TK_REG` / `')'`。

### 4.6 Parse：递归下降

每个非终结符一个函数，文法左递归 (`A = A op B | C`) 重写为循环：

```c
// add_expr = mul_expr { ("+" | "-") mul_expr }
static word_t parse_add(void) {
    word_t v = parse_mul();
    while (peek('+') || peek('-')) {
        int op = consume().type;
        word_t r = parse_mul();
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}
```

**直接在 parse 中计算求值**，不构造 AST。原因：

- 我们不需要把 AST 留存下来做优化、序列化
- 省一层数据结构，代码更少
- 如果将来要做「监视点条件跳过」之类复杂功能，再拆 AST 不迟

> **取舍标签**：pragmatic choice。AST 是理论理想，但当前 stage 不需要。

### 4.7 Primary 的三类叶子

```c
static word_t parse_primary(void) {
    Token t = consume();
    switch (t.type) {
        case TK_NUM:    return strtoul(t.str, NULL, 10);
        case TK_HEXNUM: return strtoul(t.str, NULL, 16);
        case TK_REG:    return reg_by_name(t.str + 1);  // 跳过 '$'
        case '(': {
            word_t v = parse_expr();
            expect(')');
            return v;
        }
        default:
            panic("unexpected token '%s'", t.str);
    }
}
```

`reg_by_name` 在 stage 1 只需支持 `pc` 和 `x0..x31` 的查表（CPU 状态先用一个**占位全局变量**，stage 2 再接上真实 `cpu.gpr`）。

### 4.8 解引用

```c
// unary = "*" unary | ...
case TK_DEREF: {
    word_t addr = parse_unary();
    return pmem_read(addr, 4);    // stage 2 之后才真正能读内存
}
```

Stage 1 里 `pmem_read` 也可以先返回 0 加警告，等 stage 2 真实内存到位再激活。

---

## 5. Concrete example — 走读 `*($pc + 4) == 0x00000013`

### Tokenize

```
输入:  *($pc + 4) == 0x00000013
tokens: [TK_DEREF] [ '(' ] [TK_REG "$pc"] [ '+' ] [TK_NUM "4"] [ ')' ] [TK_EQ] [TK_HEXNUM "0x00000013"]
```

注意 `*` 被 fix 成 `TK_DEREF`，因为它前面什么都没有。

### Parse 递归栈

```
parse_expr()
└─ parse_or()
   └─ parse_and()
      └─ parse_eq()           ← 看见 TK_EQ，左右两边各调一次 parse_rel
         ├─ parse_rel()
         │  └─ ... ─→ parse_unary()
         │             └─ 看见 TK_DEREF，递归
         │                └─ parse_unary()
         │                   └─ parse_primary() = '('
         │                      └─ parse_expr()
         │                         └─ parse_add() = $pc + 4
         │                      └─ expect ')'
         │                返回 addr = pc_val + 4
         │             返回 pmem_read(addr, 4)
         └─ 右侧：parse_rel() = 0x13
         比较：左值 == 0x13 ?
```

### Eval

假设 `pc = 0x80000000`，内存 `0x80000004` 处是指令 `addi x0, x0, 0`（编码 `0x00000013`）：

```
*($pc + 4) = pmem_read(0x80000004, 4) = 0x00000013
0x00000013 == 0x00000013  →  1
```

`p` 命令打印：`$1 = 1 (0x1)`。

---

## 6. Acceptance

### 6.1 表达式测试档

目录 `tests/expr/basic.txt`：

```
# 期望值    表达式
3           1+2
11          1 + 2 * 5
10          2 * (3 + 2)
1           1 == 1
0           1 != 1
1           1 && 1
0           1 && 0
5           1 << 2 | 1
0xff        0xaa | 0x55
```

测试驱动：`tests/expr/runner.sh`（或直接 C 程序）逐行 split 期望值和表达式，喂给 `temu -b --expr`（为此给 temu 加一个非交互的求值模式）或者写一个专门的单元测试 binary。

### 6.2 模糊测试（可选但强烈推荐）

`tools/gen-expr/`：生成随机合法表达式，同时用 C 编译器和我们的求值器各算一遍，对比结果。几百条随机表达式能打出 99% 的 parser bug。

```
1. 生成 "1 + 2 * 3 - 4"
2. 用临时 C 文件 + gcc 编译并运行，拿到真值 x
3. 把 "1 + 2 * 3 - 4" 喂给 temu 的 expr eval，拿到 y
4. assert x == y
```

> **PA 讲义原话的精髓**：你永远不知道自己的 parser 有多少 bug，直到你开始 fuzz 它。

### 6.3 Makefile target

```makefile
test-expr: $(TARGET)
	@./tests/expr/run.sh

test-expr-fuzz: tools/gen-expr/gen-expr $(TARGET)
	@./tests/expr/fuzz.sh 1000
```

---

## 7. Limitations / pitfalls

### 7.1 词法歧义（反复踩坑榜）

| 坑 | 怎么踩 | 怎么避 |
|----|--------|--------|
| `0x10` 被切成 `0` + `x` + `10` | HEX 规则放在 NUM 后面 | HEX 必须放 NUM 前 |
| `==` 被切成两个 `=` | `=` 单字符规则被先匹配 | 两字符 token 必须先匹配 |
| 一元 `-` 当成减法 | 看到 `-5` 先减后负 | fix-pass 改 token 类型 |
| 一元 `*` 当成乘法 | 同上 | 同上 |
| 负号和二元减号优先级 | 一元 `-` 应比 `*` 优先 | 放在 `unary` 层 |

### 7.2 整数类型

表达式里的常量、寄存器值都是 `word_t`（即 `uint32_t`）。语义上：

- **算术溢出**：按 C 的无符号回绕语义，和真 CPU 一致
- **除零**：先 `assert(rhs != 0)` 然后 `panic`，不要产生 UB
- **比较**：默认无符号比较；stage 3 讨论 `SLT` / `SLTU` 时再分

### 7.3 错误恢复

Stage 1 的 parser **不做错误恢复**：遇到语法错误直接 `panic`。用户在 REPL 里打错了就打错了，不影响 session（REPL 下一轮会重新读输入）。

> **工程现实**：做错误恢复（类 IDE）成本高，对"自己用的调试器"边际收益低。已知债。

### 7.4 监视点实现

```c
typedef struct watchpoint {
    int NO;                       // 编号
    char expr[64];                // 原始表达式字符串
    word_t last_value;            // 上次求值结果
    struct watchpoint *next;
} WP;
```

每条指令执行完后，对所有 watchpoint 重新求值，和 `last_value` 比较；不同就 halt。

> **性能注意**：有 10 个 watchpoint、跑 1M 指令就意味着 10M 次表达式求值。Stage 1 不用优化（性能测不出来），但知道这个特征，stage 5 后若感觉慢可以缓存 AST。

---

## 8. 实现步骤建议（按这个顺序写，每步都能独立测）

1. **搭 `include/common.h`**：定义 `word_t` / `paddr_t`，放 `Log` / `Assert` / `panic` 宏
2. **拆 `main.c`**：把 REPL 搬到 `src/monitor/sdb.c`，`main` 只留参数解析 + 调 `sdb_mainloop()`
3. **加 `info r` / `info w` 占位**：后两者打印 "TODO"
4. **实现 tokenize**：先只支持 NUM 和 `+-*/()`，命令 `p 1+2` 能出结果
5. **扩充 token 种类**：HEX / REG / `==` / 逻辑运算
6. **Fix 一元运算**：`-` / `*` / `!` / `~`
7. **实现 `p EXPR`**：完整 parse + eval
8. **实现 `x N EXPR`**：求值出地址后 memcpy 一段（stage 2 之前可以只 dump 占位字节）
9. **实现监视点 `w` / `d` / `info w`**
10. **写测试** `tests/expr/basic.txt` + runner
11. **（可选）模糊测试**

---

## 9. 与后续 stage 的连接

| Stage 1 留下的东西 | 谁会用它 |
|-------------------|----------|
| `expr()` 函数 | Stage 3 的单指令测试脚本；Stage 4 的 difftest 对某些表达式比值 |
| `reg_by_name` | Stage 2 的寄存器文件填好后直接接上 |
| `pmem_read` stub | Stage 2 替换成真实实现 |
| Watchpoint 机制 | Stage 3 之后调试指令执行时会频繁用 |
| 命令表里 `c` / `si` stub | Stage 3 的 `cpu_exec(n)` 填进去 |

---

## 10. 为什么这个 stage 这么"重"

看起来我们只是做了个"调试器"，但实际上学到了：

- **Tokenize + Parse** 的完整流程——这是编译器前端的核心
- **递归下降** 和文法规则的直接对应——看文法就能写 parser
- **优先级** 的表达方式：层级函数调用
- **词法歧义** 的现实解法（fix-pass）
- **测试驱动**：fuzz 一个 parser 几乎总能发现 bug

Stage 1 的含金量在整条路线里名列前茅。值得花时间。
