---
name: vix-conventions
description: Naming, structure and code conventions used across the Vix language and compiler.
---

# 代码规范

## 命名

| 种类 | 风格 | 例子 |
| --- | --- | --- |
| 函数 | snake_case | `parse_expression`、`method_resolve` |
| 变量 | snake_case | `receiver_kind`、`param_types` |
| 类型 / struct | PascalCase | `AstFunction`、`MethodTable` |
| ADT 变体 | PascalCase | `ReceiverOwned`、`ResolutionMethod` |
| 模块前缀 | 小写 | `ast_`、`semantic_`、`mir_`、`codegen_` |

编译器源码的惯例是**用模块前缀避免符号冲突**，因为整个编译单元共享一个
符号空间。新增函数请沿用所在文件的既有前缀。

## 文件结构

编译器源文件的固定开头：

```vix
#[no_main]

include "ast.vix"
include "lexer.vix"
```

- `#[no_main]` 表示这是库文件。
- import 顺序按依赖层次，不要形成环。
- 新增文件要在 `src/sys.vix` 里加 `include "..."`，否则不参与编译。

## 风格

- 大括号换行（Allman），和仓库现有代码一致。
- `if (...) { ... }` 的单行短写法在编译器源码里很常见，允许。
- 注释写**为什么**，不写**是什么**。仓库现有注释密度中等偏高，
  解释设计取舍的注释价值最高。
- 不要留 TODO 占位；要做就做完，做不完要在提交信息里说明。

## 错误处理

用户可见错误通过语义/类型/所有权检查产生，带稳定错误码：

```vix
semantic_add_error_loc(result, source, filename, "NameError", "unknown method '" + name + "'", func_name, line, col, length)
ownership_add_error(state, source, filename, "E5006", "shared reference is read-only", line, col, length)
```

错误码分段：`E1xxx` 解析、`E2xxx` 命名、`E3xxx` 类型、`E5xxx` 所有权。
批量错误要限制数量，避免刷屏。

## 硬编码红线

以下做法在本仓库被明确禁止（有门禁脚本检查其中一部分）：

- 用字符串拼接方法名去做查找，而不是查表。
- 用裸数字表示枚举语义（例如 receiver kind 写 `1/2/3`）。
- 按名字判断是不是内建方法（例如比较 `"push"`）。
- 在多个后端各写一份相同逻辑，而不是放到共享模块。
- 用「只对一个例子有效」的特判冒充通用实现。

统一放到数据/注册表模块，然后让所有消费方调用同一份 API。

## 文档

- 语言特性文档放 `docs/`，改动要同步更新。
- 每个版本一个 `docs/RELEASE_vX.Y.Z.md`。
- 面向 AI 的操作手册放 `skill/`（本目录）。
