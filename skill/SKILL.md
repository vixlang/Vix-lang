---
name: vix
description: >
  Author, review, refactor, compile, test and debug programs in the Vix
  language and in the Vix compiler itself. Load this before writing any .vix
  code, before changing the compiler, and before diagnosing a Vix build or
  language-server problem.
---

# Vix

Vix 是一门静态类型的系统编程语言，编译器自身也用 Vix 写成（自举）。
本目录是给人和 AI 共用的工作手册。

## 为什么这样组织

AI 写 Vix 时缓存命中率低，根因通常不是模型，而是**每次会话读的文件不一样、
读法不一样、试错路径不一样**，导致前缀不断变化。所以本 skill 的设计目标是
「字节级稳定的前缀 + 按需加载的细节」：

1. `SKILL.md`（本文件）保持短小、稳定，是所有会话的第一段上下文。
2. 细节放 `reference/` 和 `guides/`，需要时才读，不要一次全读。
3. 固定的**加载顺序**写在 [guides/agent-context.md](guides/agent-context.md)，
   照做可以让多次会话的前缀一致。

## 五条铁律

1. **先验证再断言。** Vix 的语法和编译器行为有明确边界，写完必须跑
   `build/vixc-patched <file> --check`，不要凭印象下结论。
2. **没有 `impl` 块。** 方法用 receiver 声明：`fn (&Point) length(): i32`。
   旧的 `impl Point { ... }` 语法已废弃。
3. **不要按名字猜语言行为。** 例如数组 `push` 是编译器内建 intrinsic，
   不是用户方法；方法解析走统一 method table，不要用字符串拼接方法名。
4. **编译产物不要进 git。** `build/`、`runtime/*.o`、`*.vsix`、`vstd` 都是本地产物。
5. **改编译器前后都要自举。** 改动 `src/` 后必须确认
   `build/vixc src/main.vix -obj -o /tmp/vixc.o` 仍然返回 0。

## 索引

语法与类型：

| 文件 | 内容 |
| --- | --- |
| [reference/language.md](reference/language.md) | 词法、语句、表达式、match、循环、闭包 |
| [reference/types.md](reference/types.md) | struct、ADT、Option、泛型、引用与指针 |
| [reference/methods.md](reference/methods.md) | receiver 方法、三种 receiver、调用与解析 |
| [reference/modules-and-macros.md](reference/modules-and-macros.md) | import/mod/pub、宏系统 |
| [reference/ffi-and-runtime.md](reference/ffi-and-runtime.md) | extern "C"、内存、字符串、运行时 |

工程方法：

| 文件 | 内容 |
| --- | --- |
| [guides/conventions.md](guides/conventions.md) | 命名、结构、代码规范 |
| [guides/compile.md](guides/compile.md) | 编译器 CLI、后端、macOS 链接 |
| [guides/test.md](guides/test.md) | 测试入口与怎么写回归 |
| [guides/debug.md](guides/debug.md) | 诊断格式、错误码、调试手段 |
| [guides/reading.md](guides/reading.md) | 怎么读这个仓库（含编译管线地图） |
| [guides/refactoring.md](guides/refactoring.md) | 重构路子与安全顺序 |
| [guides/agent-context.md](guides/agent-context.md) | AI 工作流与上下文纪律 |
| [checklists/definition-of-done.md](checklists/definition-of-done.md) | 收工前检查表 |

本目录有校验脚本，改了文档就跑：

```sh
python3 scripts/check-skill-docs.py
```

它会检查相对链接、内联引用的仓库路径是否存在，
并**编译文档里所有含 `fn main` 的 Vix 代码块**——示例不能编译就算失败。

## 最短可用上下文

只想快速写一个能跑的 Vix 程序，读完这一节就够：

```vix
type Point = struct {
    x: i32,
    y: i32
}

fn (&Point) length(): i32 {
    return self.x + self.y
}

fn (&mut Point) shift(dx: i32) {
    self.x = self.x + dx
}

fn make(x: i32, y: i32): Point {
    return Point{ x: x, y: y }
}

fn main(): i32 {
    let mut p = make(3, 4)
    p.shift(1)
    print(p.length())
    return 0
}
```

要点：`fn name(a: T): R { return ... }`、`type X = struct { ... }`、
`let` / `let mut`、`print(...)`、方法用 receiver 声明且方法体内用 `self`。
