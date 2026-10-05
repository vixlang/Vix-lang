---
name: vix-agent-context
description: Context discipline for AI agents working on Vix, tuned for prompt cache hit rate.
---

# AI 工作流与上下文纪律

这一节直接针对「写 Vix 时 token 缓存命中率低」。

## 为什么会命中率低

前缀缓存要求**每次请求的开头字节尽量相同**。写 Vix 时常见的破坏源：

1. 每次会话读的文件不同、顺序不同 —— 前缀根本对不齐。
2. 一上来读整个大文件（`parser.vix` 2600 行、`analysis.vix` 上万行），
   把稳定前缀挤掉。
3. 把大段编译日志、IR、测试输出贴进上下文 —— 体积大且每次都不同。
4. 靠试错发现「macOS 上 `-o` 不能链接」这类事实，每次会话重来一遍。

第 4 条最贵：试错过程产生的中间输出全是缓存不友好的。

## 固定加载顺序

按这个顺序读，且**只读到够用为止**：

```text
1. skill/SKILL.md                  （总是）
2. skill/reference/language.md     （写代码时）
3. skill/reference/types.md        （涉及类型/泛型/ADT 时）
4. skill/reference/methods.md      （涉及方法时）
5. skill/guides/compile.md         （要编译/运行时）
6. skill/guides/test.md            （要验证时）
7. skill/guides/debug.md           （遇到报错时）
8. skill/guides/reading.md         （要改编译器时）
```

写一个新 Vix 程序，1+2 通常就够。改编译器才需要 4+8+9。

## 上下文纪律

**要做的**

- 用 grep 定位函数，再读那一段，而不是读整个文件。
- 大文件用 `offset` + `limit` 分段读。
- 编译输出只保留**错误行**：`... 2>&1 | grep -E "error\[|undefined"`。
- 长任务把日志写到 `/tmp`，只读摘要行。
- 同一事实只确认一次，之后直接用（例如「`--self` 后端在 arm64 不可用」）。

**不要做的**

- 不要把完整 LLVM IR / 完整测试输出贴进对话。
- 不要反复读刚读过的同一个文件。
- 不要在没跑 `--check` 的情况下猜 Vix 语法是否合法。
- 不要靠试错找编译方式，直接用 [compile.md](compile.md) 里的命令。

## 一次会话的推荐开场

```text
1. 读 skill/SKILL.md
2. 读 skill/reference/language.md
3. 如果是改编译器，再读 skill/guides/reading.md
4. 声明要改的文件范围，然后开始
```

## 已知环境事实（省掉重复探测）

| 事实 | 结论 |
| --- | --- |
| 主机 | macOS arm64 |
| `--backend=self` | **不可用**，报 `self backend only supports x86_64 targets` |
| `vixc -o` 链接 | **不可用**，内置链接器只有 ELF/COFF |
| 可用的运行方式 | LLVM 后端 `-obj` + `clang++` 链接；用 `scripts/run-vix.sh` |
| 链接器 | 用 `clang++`，用 `clang` 会报 `-lto_library` 错 |
| 每文件链接告警 | `ld: warning: built for newer macOS version` —— 无害，扩展不会误报 |
| 引导编译器 | `build/vixc`（编译 `src/` 用） |
| 新编译器 | `build/vixc-patched`（跑测试用），由 `scripts/build-analyzer.sh` 产出 |
| analyzer 二进制 | `build/vix-analyzer` |
| 编辑器扩展 | `editors/vscode/vix-analyzer-0.8.3.vsix` |

## 每次改完必须跑的

```sh
build/vixc src/main.vix -obj -o /tmp/vixc.o     # 自举
sh scripts/build-analyzer.sh                     # analyzer 构建
git diff --check                                 # 空白/冲突标记
```

涉及方法再加：

```sh
sh scripts/check-method-invariants.sh
python3 tests/methods_e2e.py
```

## 汇报纪律

- 只报告**工具结果能证明**的事情。
- 没验证的写「没验证」，不要用「应该」「大概」蒙过去。
- 已知限制要主动说（例如「按值 struct 的 use-after-move 未实现」）。
- 不要把「能跑一个例子」说成「完整实现」。
