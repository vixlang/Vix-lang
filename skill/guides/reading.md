---
name: vix-reading
description: How to navigate the Vix compiler source tree and its compilation pipeline.
---

# 怎么读这个仓库

## 三层结构

```text
src/            编译器本体（Vix 写的，自举）
vix-analyzer/   语言服务器（LSP），复用 src/ 的 lexer/parser/semantic
editors/vscode/ VS Code 扩展（TypeScript），内嵌 vix-analyzer 二进制
```

周边：`tests/` 测试、`examples/` 示例、`docs/` 文档、`scripts/` 脚本、
`lib/` C/C++ 支撑（LLVM、链接器）、`runtime/` 运行时、`vstd/` 标准库（子模块）。

## 编译管线

`src/main.vix` 的 `run_mode` 是唯一入口，顺序：

```text
读文件
  -> preprocess        宏展开、import 收集      src/preprocess.vix
  -> lex               分词                      src/lexer.vix
  -> parse             AST                       src/parser.vix
  -> module graph      多文件合并                src/module.vix
  -> method attach     建方法表                  src/method.vix
  -> semantic          命名解析、作用域           src/semantic.vix
  -> typecheck         HM 类型推断               src/infer/solver.vix
  -> desugar           ADT 展开、泛型单态化       src/desugar.vix
  -> typed analysis    所有权 / 类型化 IR         src/analysis/typed_analysis.vix
  -> lower for backend 类型串降级                 src/analysis/typed_analysis.vix
  -> MIR / LIR         src/mir/
  -> LLVM IR           src/codegen.vix
  -> 链接              src/backend/linker.vix
```

**关键**：`analysis_lower_program_for_backend` 把源码类型串（`&T`）转成后端形式
（`refptr:` / `ptr:`）。任何跨阶段携带的类型字符串都要在这里一起降级，
否则后端会拿到源码形式而 ABI 错位。

## 核心数据结构

| 类型 | 位置 | 说明 |
| --- | --- | --- |
| `AstProgram` | `src/ast.vix` | 一次编译的全部顶层声明 + 两个 store |
| `ExprStore` | `src/ast.vix` | 表达式数组，**也是方法表的载体**（`methods` 字段） |
| `StmtStore` | `src/ast.vix` | 语句数组 |
| `AstFunction` | `src/ast.vix` | 函数/方法；方法多出 `receiver_kind` / `receiver_type` |
| `AstExpr` | `src/ast.vix` | 通用表达式节点，用 `tag` 区分，`method_call` 是其中一种 |
| `MethodTable` | `src/method_decl.vix` | 方法声明索引 |
| `MethodResolution` | `src/method_decl.vix` | 一次解析的完整结果 |

`AstExpr` 是**通用节点**，很多字段复用：
`left_idx`（左操作数 / receiver）、`right_idx`、`args`、`text`（名字）、
`field_names`、`type_args`、`option_value`、`capture_names`。
读代码时先看 `tag`，再看该 tag 用哪些字段。

## 读代码的顺序建议

改某个语言特性时：

1. `src/parser.vix` 找到解析点，确认 AST 怎么存。
2. `src/ast.vix` 看节点定义。
3. `src/semantic.vix` 看名字/作用域检查。
4. `src/infer/solver.vix` 看类型规则。
5. `src/analysis/typed_analysis.vix` 看所有权与类型化 IR。
6. `src/mir/mir.vix` 和 `src/codegen.vix` 看降级。
7. 两端都要动的话，抽共享模块（参考 `src/method.vix` 的做法）。

## 不要做的事

- 不要一上来读整个文件。`src/parser.vix` 2600 行、`vix-analyzer/analysis.vix`
  上万行，先 grep 定位函数再读片段。
- 不要假设「聚合能过」等于「单文件没问题」：很多文件单独 `--check` 会报缺符号，
  这是 import 聚合方式导致的，不是 bug。
- 不要去动 `build/`、`runtime/*.o`、`vstd`、`.idea/`、`.vscode/`，
  这些是本地状态。
