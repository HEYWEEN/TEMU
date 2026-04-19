# TEMU 文档

本目录放项目的**教科书式说明**：每个 stage 一份，讲清楚「为什么这样做 / 怎么做 / 怎么验证 / 常见坑在哪」。

---

## 阅读顺序

1. 根目录 [README.md](../README.md)：项目总览和路线图
2. [CLAUDE.md](../CLAUDE.md)：工程规范（命名、错误处理、日志、测试档次）
3. 当前所在 stage 的文档（按 `../README.md` 的状态列找）
4. 下一个 stage 的文档（提前 1 个 stage 预读，了解即将用到的概念）

**不建议**一次读完所有 stage 文档——后面的文档会依赖前面 stage 已建立的词汇和代码。

---

## 每份 Stage 文档的结构

参照用户 `learning.md` 的 Explanation Hierarchy，每份文档尽量按以下顺序展开：

1. **Problem motivation** — 这个 stage 解决什么问题？不做会怎样？
2. **Naive solution** — 朴素方案是什么？为什么不够？
3. **Key idea** — 关键设计洞察
4. **Mechanism** — 逐步工作原理
5. **Concrete example** — 真实场景走读（非玩具例子）
6. **Acceptance** — 验收标准与测试命令
7. **Limitations / pitfalls** — 局限 + 常见坑

部分文档会额外给：
- **API 契约**（函数签名 + pre/post condition）
- **数据结构定义**
- **与操作系统 / 计组课知识的连接点**

---

## 文档写作约定

- 中文叙事，英文代码和术语（寄存器名、指令助记符、字段名都不翻译）
- 代码块一定可复制、可跑（或可汇编）
- 关键洞察用引用 `> ` 突出
- 权衡用表格列出（维度、选项 A、选项 B、理由）
- 「工程 hack」/「理论理想」/「已知债」显式标注
- 图不多画，用 ASCII 框图足矣

---

## Stage 文档列表

| Stage | 文件 | 状态 |
|-------|------|------|
| 0 | [stage-0-skeleton.md](stage-0-skeleton.md) | ✅ 已完成 |
| 1 | [stage-1-monitor.md](stage-1-monitor.md) | ⏳ 待实现 |
| 2 | [stage-2-memory-cpu.md](stage-2-memory-cpu.md) | 📝 设计稿 |
| 3 | [stage-3-rv32i.md](stage-3-rv32i.md) | 📝 设计稿 |
| 4 | [stage-4-difftest.md](stage-4-difftest.md) | 📝 设计稿 |
| 5 | [stage-5-mmio.md](stage-5-mmio.md) | 📝 设计稿 |
| 6 | [stage-6-os.md](stage-6-os.md) | 📝 可选 |

> **说明**：「设计稿」表示文档已勾出动机、关键概念、API 草案与验收标准，但实现细节会在该 stage 真正开始时进一步细化——避免提前写出与最终代码不一致的内容。
