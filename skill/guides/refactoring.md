---
name: vix-refactoring
description: How to refactor the Vix compiler safely, including the shared-module pattern.
---

# 重构

## 安全顺序

1. **先建可验证基线。** 把当前编译器复制一份：
   `cp build/vixc-patched /tmp/vixc-before`。
2. **小步改，每步都自举。**
   `build/vixc src/main.vix -obj -o /tmp/vixc.o` 必须一直是 0。
3. **每步都编译 analyzer。** `sh scripts/build-analyzer.sh`。
   analyzer 有自己的聚合路径，只测编译器会漏掉 import/依赖问题。
4. **每步都做编译差分**（见 [test.md](test.md)），确认没有 `old=0 new=1`。
5. **一步一个提交**，提交信息写清「为什么」。

## 什么时候该抽共享模块

出现下列任一情况，就说明有重复逻辑，该抽出去：

- 同一个概念在 2 个以上阶段各自算了一遍。
- 一份逻辑在多个后端各写一份（solver / ownership / MIR / LLVM）。
- 某个字符串/数字约定在多处被手工重建。

参考已完成的案例——方法体系的重构（提交 `e1d7608`）：

| 之前 | 之后 |
| --- | --- |
| 各阶段 `receiver + "::" + name` 拼名字 | `method_qualify` 单点生成 |
| 后缀扫描查方法 | `MethodTable` 结构化查找 |
| receiver kind 用裸数字 `1/2/3` | `ReceiverKind` ADT |
| `push` 在三处按名字判断 | `src/intrinsic.vix` 注册表 |
| 调用方自己算 receiver 下标 `i+1` | `method_signature_user_arg_type` |

## 加一个新语言特性的完整清单

1. `src/ast.vix` 加节点字段（注意所有构造器和 `ast_function_with_loc` 这类
   复制函数都要带上新字段，**漏一个就会静默丢数据**）。
2. `src/parser.vix` 解析。
3. `src/module.vix` 合并逻辑保留新字段。
4. `src/desugar.vix` 重建 AST 时保留新字段
   （历史上这里丢过 receiver 元数据，是个真实 bug）。
5. `src/semantic.vix` / `src/infer/solver.vix` 语义与类型。
6. `src/analysis/typed_analysis.vix` 所有权与 backend lowering。
7. `src/mir/mir.vix` + `src/codegen.vix` 后端。
8. analyzer 的 stub/符号/查询。
9. 测试 + 文档 + 门禁脚本。

## 改动风险表

| 改动 | 高风险点 |
| --- | --- |
| 给 `AstFunction` / `AstExpr` 加字段 | 复制点漏字段 → 静默丢数据 |
| 给 `AstProgram` / `ExprStore` 加字段 | 所有构造器要更新；desugar 会重建 program |
| 改类型串约定 | 后端 lowering 必须同步，否则 ABI 错位/段错误 |
| 新增编译器源文件 | 必须加进 `src/sys.vix`，并统一 import 拼写 |
| 改 symbol mangling | 影响导出符号与 golden 测试，非必要不动 |

## Vix 语言的语法限制（会绊住重构）

- **不支持 `local[i].field = value`。** 只支持 `struct.field[i].field = value`
  这种从标识符开始的字段链。要在循环里改数组元素结构体字段，
  要么用字段链写法，要么取出→改→写回。
- 单文件 `--check` 对依赖多的文件不成立，别据此判断重构是否破坏。
- 全局变量对复杂聚合类型支持有限（会生成非法 IR），
  共享状态优先放进已有的 store/struct 里传递，而不是新开模块级全局。
  方法表就是因此挂在 `ExprStore` 上的。

## 提交纪律

- 只提交相关文件。`build/`、`runtime/*.o`、`*.vsix`、`vstd`、`.idea/`、`.vscode/`
  一律不进提交。
- 提交信息写清楚行为变化和原因，尤其是修掉的 bug。
- 推送前跑：自举 + analyzer 构建 + 相关门禁 + `git diff --check`。
