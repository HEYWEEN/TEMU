# 教科书代码验证

每一章里出现的可编译代码片段，都要有对应的 `verify-PN.sh` 脚本——
从 `.md` 中抽出片段、组装、编译、跑 smoke test。

## 为什么要做

教科书最常见的问题是"书里的代码和 repo 的代码脱节"。随着 TEMU
迭代，早期章节的示例会变得可能**读起来对、跑起来错**。这个目录的
任务就是**每次发布前重跑所有验证脚本**，挂掉就说明教科书某一章
需要同步更新。

## 使用

```bash
# 跑单章
bash docs/textbook/verify/verify-p0.sh

# 跑全部
for s in docs/textbook/verify/verify-*.sh; do
    bash "$s" || { echo "FAIL: $s"; exit 1; }
done
```

## 约定

- 验证脚本在 `/tmp/textbook-verify/pN/` 搭独立工作区，不污染主 repo
- smoke test 用输入/输出对比（`printf ... | ./build/temu` + `grep`）
- 退出码 0 = 全过；非零 = 具体哪个 case 挂了
