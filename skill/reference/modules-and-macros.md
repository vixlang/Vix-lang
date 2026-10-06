---
name: vix-modules-and-macros
description: Vix imports, modules, visibility and the macro system.
---

# 模块与宏

## 包含与模块

两种机制，两个关键字：

```vix
include "util/fresh.vix"        // 把文件源码拼进来，扁平命名空间
use "lib/math.vix" as math      // 加载模块，符号加 math:: 前缀
```

- `include` 走预处理器文本拼接；`use` 走模块图并加命名空间前缀。
- **模块身份是路径，命名空间是别名**，两者互不推导。`as` 必写，省略报错。
- 路径按声明所在文件的目录解析，并做词法规范化（`./a.vix`、`sub/../a.vix`
  与 `a.vix` 同为一个文件，只包含一次）。
- 同一文件可以用不同别名多次 `use`，只加载一次。
- **没有包管理器**：没有裸名展开、没有 `$VIX_HOME`、没有 `.vix/libs`。
  完整规则见 [../../docs/MODULES.md](../../docs/MODULES.md)。
- 编译器的聚合入口是 `src/main.vix`，它 `include "sys.vix"`，后者列出全部源
  文件。新增编译器源文件要在 `src/sys.vix` 加一行 `include "x.vix"`。
- 旧语法（`mod` / `import`）已删除，但会给出迁移提示而不是语法错误。

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
