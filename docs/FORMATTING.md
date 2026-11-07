# 格式化

Vix 自带格式化器，命令行和语言服务器共用同一份实现，所以编辑器和 CLI
永远不会给出不同结果。

## 用法

```sh
# 就地格式化（和 rustfmt 一样）
sh scripts/vix-fmt.sh file.vix

# 整个目录
sh scripts/vix-fmt.sh src/

# CI：不改文件，只报告哪些文件需要格式化
sh scripts/vix-fmt.sh --check src/
```

等价的原生命令：

```sh
build/vixc-patched file.vix --fmt                  # 就地重写
build/vixc-patched file.vix --fmt -o out.vix       # 写到别的路径
build/vixc-patched file.vix --fmt -o -             # 输出到 stdout
build/vixc-patched file.vix --fmt-check            # 未格式化则退出码非 0
```

## 配置

命令行参数：

| 参数 | 说明 |
| --- | --- |
| `--max-width=N` | 每行最多 N 列，`0` 表示不换行（默认 100） |
| `--indent=N` | 每级缩进 N 个空格，范围 1..16（默认 4） |
| `--use-tabs` / `--spaces` | 用制表符或空格缩进 |

也可以在源文件旁边放 `vix-fmt.toml`：

```toml
# 行宽上限，0 表示不换行
max_width = 100

# 每级缩进的空格数
indent_width = 4

# 是否用制表符缩进
use_tabs = false
```

命令行参数优先于配置文件。编辑器里缩进宽度来自 VS Code 自己的
`editor.tabSize` / `editor.insertSpaces`，行宽来自 `vix-fmt.toml`——
因为 LSP 的 FormattingOptions 只带缩进信息。

## 它做什么

1. **按括号深度重排缩进**。字符串、字符、注释里的括号不参与计算。
2. **去行尾空白**。
3. **整理空行**：连续空行折叠成一个，顶层声明之间保留一个空行。
4. **规范化行内空格**：
   - 逗号后一个空格，前面没有空格
   - 类型标注 `name: T` 冒号前无空格、后一个空格；`::` 和 `:[T]` 不动
   - 二元运算符两侧各一个空格
   - `and` / `or` 两侧各一个空格
   - `)` 前、`(` 后不留空格；`fn (` 的 receiver 空格保留
5. **超宽行换行**：
   - 优先在**参数/字段列表**处断开（取最后一层含逗号的括号组，所以
     `fn name:[T](a, b)` 会断在实参表而不是泛型参数表）
   - 否则在**二元运算符**处断开，且整条链用同一个续行缩进（不会阶梯式右移）
   - 都断不了就把这一行留着——猜错比留长更糟

## 设计取舍

- **不重新打印 AST**。AST 不携带注释，从 AST 重排会删掉所有注释。
  因此格式化器只用 parser 判断文件是否合法，其余工作在原始字节上做。
- **不能断的行保持原样**。例如只有一个超长参数的函数签名会被保留。
- **幂等**。`fmt(fmt(x)) == fmt(x)` 是硬性验收项。单趟规则之间会互相影响：
  重排缩进可能让某行变长，换行又会把文本挪到新行，而新行的缩进取决于前面
  行打开了什么括号。因此 `fmt_format` 会**把自己的输出再跑一遍，直到不再
  变化**（有轮数上限），返回的是不动点。正常情况一轮就稳定。
- **语法错误时拒绝格式化**，避免把写了一半的缓冲区按猜测重排。
- **块注释内容不动**，只重写它的缩进。

## 在编辑器里

「格式化文档」（`Shift+Alt+F` / 命令面板 `Format Document`）走 LSP 的
`textDocument/formatting`，和 CLI 是同一份实现。

> 历史问题：语言服务器实现了这个请求，但 initialize 结果里没有声明
> `documentFormattingProvider`，导致编辑器从不发请求、按钮点了没反应。
> 现在已声明，并有 `vix-analyzer/tests/lsp_formatting.py` 守住。

## 测试

```sh
python3 tests/fmt_e2e.py                              # 幂等、列宽、缩进、check 模式
python3 vix-analyzer/tests/lsp_formatting.py build/vix-analyzer
python3 vix-analyzer/tests/lsp_format_safety.py build/vix-analyzer
```

最强的语义验证是：把整个 `src/` 格式化一遍，然后确认编译器仍然能自举
（`build/vixc src/main.vix -obj`）。格式化改变语义的话这一步必然失败。

## 已知限制

- 不会把已经跨行的结构合并回一行（行级格式化器不重排跨行结构）。
- 单个超长且没有逗号、没有可断二元运算符的行会保持超宽。
- 对齐用的多余空格会被折叠成一个（这与 rustfmt 行为一致）。
