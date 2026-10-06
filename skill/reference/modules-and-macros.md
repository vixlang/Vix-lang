---
name: vix-modules-and-macros
description: Vix imports, modules, visibility and the macro system.
---

# 模块与宏

## 导入

```vix
import "util/fresh.vix"    // 文本包含，相对导入文件所在目录
mod "parser.vix"           // 聚合：把文件并入当前编译单元
mod sys                    // 具名模块（查找 sys.vix 或 sys/mod.vix）
```

- 带引号的 `import` 走预处理器：目标文件处理后的源码**文本拼进来**，
  共享扁平命名空间。`mod "x.vix"` 走模块图，会加命名空间前缀。
- 路径只按**相对导入文件的目录**解析。**没有包管理器**：裸名展开、
  `$VIX_HOME`、`.vix/libs` 这套搜索顺序**在实现里不存在**（历史上文档写过，
  但从未实现，现已删除该文档）。
- 路径会被词法规范化：`./a.vix`、`sub/../a.vix`、`a//b.vix` 与 `a.vix`
  视为同一文件，只包含一次。
- `import module::symbol` **不是有效语法**，解析阶段直接报错。跨模块调用
  一律写限定名 `module::symbol(...)`。
- 编译器的聚合文件是 `src/sys.vix`，新增编译器源文件要加一行 `mod "x.vix"`。

## 可见性

`pub` 标记导出。编译器源码里基本所有跨文件函数都是 `pub fn`。

## 编译器源码的命名空间特点

整个编译单元（`sys.vix` 聚合后的所有文件）**共享一个符号空间**，
所以文件里可以调用别处定义的函数而不再单独 import；但为了
`--check` 单文件和 analyzer 聚合能解析，**新文件仍要显式 import 依赖**。

路径已做词法规范化（见上），所以 `"ast.vix"` 和 `"../ast.vix"` 不会再被当成
两份。但**建议全仓库保持同一种拼写**，可读性更好：

- `src/*.vix` 之间用裸名 `import "ast.vix"`；
- `src/*/**.vix` 引用新方法模块时用 `import "method.vix"`，
  跨目录用相对路径（如 `src/infer/solver.vix` 里的 `import "../ast.vix"`）。

加新模块时改完全量编译 + analyzer 编译都过一遍。

## 宏

定义，名字以 `$` 开头：

```vix
macro $make_add(x: ident)
{
    fn $x(a: i32, b: i32): i32 {
        return a + b
    }
}

macro $vec[elems: expr*]
{
    [$(elems),*]
}
```

调用：

```vix
$make_add(my_add)          // 圆括号调用
let arr = $vec[1, 2, 3]    // 方括号调用
```

- 参数类型标注：`ident`、`expr`；`expr*` 表示重复参数。
- 宏体内 `$name` 展开参数，``$(name),*`` 这种形式配合 `*` 做重复展开。
- 宏有卫生性（hygiene）和递归深度限制，超限会报错。
- 宏在预处理阶段展开，位置映射保留，所以报错能指回源位置。

## 预处理器行为

- 字符串形式的 `import` 会被预处理阶段处理，行号映射会相应调整。
- 宏展开后的诊断指向宏展开点，`--diag-format` 可切换格式。
- 若某文件依赖宏或 import，单独 `--check` 可能报缺失符号；
  这时改用聚合入口（例如 `src/main.vix`）验证。
