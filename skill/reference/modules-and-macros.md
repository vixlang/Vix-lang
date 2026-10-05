---
name: vix-modules-and-macros
description: Vix imports, modules, visibility and the macro system.
---

# 模块与宏

## 导入

```vix
import "std/io.vix"        // 相对/搜索路径文件
import "os"                // 裸名，按包规则展开
mod "parser.vix"           // 聚合：把文件并入当前编译单元
mod sys                    // 具名模块
```

- `import` 走预处理器和文件搜索，`mod "x.vix"` 直接并入（不预处理）。
- 路径解析优先级见 [../../docs/import-resolution.md](../../docs/import-resolution.md)。
  裸名会先做包名展开（`name` 变成 `github.com/vixlang/vlib-name` 之类），
  再依次找 `.vix/libs/`、`$VIX_HOME/libs/`、`$VIX_HOME/std/`。
- 编译器的聚合文件是 `src/sys.vix`，新增编译器源文件要加一行 `mod "x.vix"`。

## 可见性

`pub` 标记导出。编译器源码里基本所有跨文件函数都是 `pub fn`。

## 编译器源码的命名空间特点

整个编译单元（`sys.vix` 聚合后的所有文件）**共享一个符号空间**，
所以文件里可以调用别处定义的函数而不再单独 import；但为了
`--check` 单文件和 analyzer 聚合能解析，**新文件仍要显式 import 依赖**。

跨目录 import 的拼写要一致，否则同一文件会被当成两份包含进来，
报 `duplicate struct`。仓库现状：

- `src/*.vix` 之间用裸名 `import "ast.vix"`；
- `src/*/**.vix` 引用新方法模块时也用裸名 `import "method.vix"`
  （依赖搜索路径回退）；
- 老文件里 `src/analysis/typed_analysis.vix` 用 `"../ast.vix"` 这种写法。

加新模块时**全仓库用同一种拼写**，改完全量编译 + analyzer 编译都过一遍。

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
