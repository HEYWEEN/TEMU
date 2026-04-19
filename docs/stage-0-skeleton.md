# Stage 0 — 骨架与 REPL

> **状态**：✅ 已完成
> **验收命令**：`make run`（进入提示符）；输入 `q` 干净退出

---

## 1. Problem motivation

还没开始写 CPU、没开始写内存，为什么先花一整个 stage 做个空壳？

因为我们需要一个**可以立即运行的最小产品**。只要 `make && ./build/temu` 能跑起来，后续每一行代码加进来都有一个「跑给自己看」的反馈闭环。反过来，如果一口气把内存、CPU、REPL 都写了再编译，编译错误一堆、运行直接崩，调试成本爆炸。

这也是 incremental delivery 的一条铁律：**先有一个能跑的空壳，再往里填肉**。

---

## 2. Naive solution & why not

> 直接在 `main` 里 `printf("hello\n")` 然后退出。

不够，因为：

- 没有交互能力 → 未来 stage 1 需要一个命令行调试器
- 没有参数解析 → 未来 stage 2+ 需要 `-b`（批量模式自动跑完）和 `-l`（写日志到文件）
- 没有测试入口 → 至少要有 `make test` 占位，方便 CI 和阶段验收

所以「空壳」不是 hello world，而是**骨架 + REPL + 参数解析**的三件套。

---

## 3. Key idea

> 把程序切成三个关注点：**参数解析**、**REPL 循环**、**命令表**。

命令表用 `{name, desc, handler}` 的数组描述，新增命令只需要加一行——这是 stage 1 大规模加命令时的必备结构。

---

## 4. Mechanism

### 4.1 程序入口

```c
int main(int argc, char *argv[]) {
    int r = parse_args(argc, argv);   // 解析 -h / -b / -l / 位置参数
    if (r != 0) return r < 0 ? 1 : 0;

    if (batch_mode) {                 // -b: 未来会在这里直接调 cpu_exec(-1)
        /* 打印占位信息并退出 */
        return 0;
    }

    repl();                           // 默认进入交互 REPL
    return 0;
}
```

### 4.2 REPL 主循环

```c
while (1) {
    printf("(temu) ");
    fflush(stdout);                       // 必须 flush，否则提示符不显示
    if (!fgets(line, sizeof line, stdin)) break;   // EOF -> Ctrl-D 退出

    char *cmd  = strtok(line, " \t\n");   // 首个 token 是命令
    if (!cmd) continue;                   // 空行
    char *args = strtok(NULL, "");        // 剩余作为 args

    dispatch(cmd, args);
}
```

### 4.3 命令分派

```c
typedef struct {
    const char *name;
    const char *desc;
    int (*handler)(char *args);           // 返回 -1 表示退出 REPL
} cmd_t;

static cmd_t cmd_table[] = {
    { "help", "Show command list", do_help },
    { "q",    "Quit TEMU",         do_q    },
};
```

Stage 1 会在这张表里加 `c` / `si` / `info` / `x` / `p` / `w` / `d`。

---

## 5. Concrete example

```text
$ make run
cc -std=c11 -Wall -Wextra -Werror -g -O0 -MMD -MP -c src/main.c -o build/main.o
cc  build/main.o -o build/temu
./build/temu
TEMU — TErminal Machine Emulator (RV32I)
Stage 0: skeleton REPL. Type 'help' for commands, 'q' to quit.
(temu) help
  help    Show command list
  q       Quit TEMU
(temu) foo
Unknown command: foo
(temu) q
$
```

---

## 6. Acceptance

| 检查项 | 命令 | 预期 |
|--------|------|------|
| 能编译（零警告）| `make` | 无 warning，产出 `build/temu` |
| REPL 能进入 | `make run` | 出现 `(temu)` 提示符 |
| `help` 可用 | REPL 中 `help` | 列出所有命令 |
| `q` 干净退出 | REPL 中 `q` | `main` 返回 0 |
| EOF 退出 | Ctrl-D | 打印换行后退出 |
| 批量模式 | `./build/temu -b` | 打印占位信息，退出 0 |
| `-h` | `./build/temu -h` | 打印 usage，退出 0 |

---

## 7. Limitations / pitfalls

### 7.1 macOS 的 `getopt` 不 permute

```bash
./temu -b image.bin         # OK: -b 在前
./temu image.bin -b         # 在 macOS 上 -b 会被当作位置参数
```

> **工程现实**：BSD `getopt` 遇到第一个非选项就停止解析，GNU `getopt` 会 permute。usage 字符串写成 `[OPTION]... [IMAGE]` 即可。Linux 下想换行为可以设环境变量 `POSIXLY_CORRECT` 或不设。

### 7.2 为什么不用 `readline`

历史命令、方向键、Tab 补全用 `readline` 几行就能做完。但：

- `readline` 不是 libc 一部分，要额外装（macOS 有 `libedit` 兼容层，但 API 细节不同）
- Stage 0 不值得让「依赖」这件事提前复杂化

> **取舍标签**：pragmatic choice。Stage 1 开始如果嫌交互不爽，可以再引入——那时已经验证了所有核心功能。

### 7.3 `fgets` 的坑

- 输入超过缓冲区会被截断到下一次读取 → 当前 `256` 字节足够，若要长命令得分块读
- 忘了 `fflush(stdout)` 则提示符不会显示（stdout 行缓冲，但重定向时变成全缓冲）

---

## 8. 与后续 stage 的连接

| 本 stage 留下的接口 | 将被谁用 |
|--------------------|----------|
| `cmd_table` 和 `dispatch` | Stage 1 加 `c` / `si` / `info` / `p` / `w` / `d` 等命令 |
| `batch_mode` 分支 | Stage 3 在 `-b` 时调用 `cpu_exec(-1)` 跑完退出 |
| `-l FILE` 参数 | Stage 2+ 的 Log 机制往文件里写 |
| `image_file` 位置参数 | Stage 2 用 `fread` 加载到 `pmem` |

---

## 9. 回顾：学到了什么

- **最小可运行产品**的价值：反馈回路越短，后续每个功能加进来信心越足
- **命令表驱动**的 REPL 是未来加命令的地基，不要写成 if-else 链
- **参数解析** 用 `getopt`：标准、熟悉、不重造；要 permute 就换 `getopt_long`
- 工具链严格性：`-Wall -Wextra -Werror` 从 stage 0 打开，让未来每次编译都在最严格档位
