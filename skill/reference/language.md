---
name: vix-language
description: Vix lexical rules, statements, expressions, match, loops and closures.
---

# 语法

本文件所有例子都可直接放进 `.vix` 文件编译。不确定时用
`build/vixc-patched <file> --check` 验证。

## 词法

关键字（`src/lexer.vix` 的 `is_keyword` 是权威来源）：

```text
fn return let mut if elif else while for in match struct type pub
import extern break continue mod as
```

注意 `macro`、`ref`、`print`、`clone`、`nil` 不在关键字表里，它们是普通标识符
或由 parser 特判。

注释：`//` 行注释，`/* */` 块注释，`/** */` 文档注释。
注释里 `TODO` `FIXME` `XXX` `NOTE` `HACK` 会被编辑器高亮。

字面量：十进制整数、浮点 `1.5`、十六进制 `0xFF`、下划线分隔 `1_000`、
字符串 `"text"`、字符 `'a'`、布尔 `true` / `false`、空指针 `nil`。

类型标注用 `:`，注意和泛型实参的 `:[...]` 区分。

## 函数

块体：

```vix
fn add(a: i32, b: i32): i32
{
    return a + b
}
```

表达式体，用 `=>`（不是 `->`，写错会得到 E1001）：

```vix
fn square(x: i32): i32 => x * x
```

无返回值写 `void` 或直接省略返回类型。变参：

```vix
extern "C"
{
    fn printf(fmt: string, ...): i32
}
```

## 变量与赋值

```vix
let answer = 42
let mut total = 0
total += 1
let name: string = "vix"
```

- `let` 不可变，`let mut` 可变。
- 需要被 `&mut` 方法接收、或要重新赋值，必须 `let mut`。
- 类型标注可选，编译器会推断。
- 类型断言写在表达式后面：`let ok = Ok(42) : Result:[i32, string]`。

## 控制流

```vix
if (x > 0) { print("pos") }
elif (x == 0) { print("zero") }
else { print("neg") }

while (i < 10) { i += 1 }

for (i in 0 .. 10) { print(i) }            // 范围，左闭右开
for (value in [1, 2, 3]) => total += value  // 遍历，按索引展开

break
continue
```

`for (x in collection)` 会被展开成按下标遍历，因此集合必须支持 `.length`
和 `collection[i]`。

布尔运算用 `and` / `or`，不是 `&&` / `||`。

## match

语句形式，分支用换行分隔，不需要逗号：

```vix
match result
{
    Valid(value) -> output = value
    Invalid(_) -> output = 1
}
```

表达式形式，可以直接 `return`：

```vix
fn pick(value: ?i32, fallback: i32): i32
{
    return match value {
        Some(inner) -> inner
        None -> fallback
    }
}
```

通配用 `_`。ADT 带载荷的分支用 `Variant(binding)`。

## 管道

`|>` 把左边的值作为右边调用的第一个实参：

```vix
let result = 21 |> double |> validate
```

## 闭包

```vix
fn apply:[T, U](value: T, transform: fn(T): U): U
{
    return transform(value)
}

fn main(): i32
{
    let offset = 2
    let a = apply[i32, i32](40, fn [offset](value: i32): i32 => value + offset)
    let b = apply[i32, bool](42, fn (value: i32): bool => value > 0)
    return 0
}
```

- `fn (params): ret => expr` 是闭包字面量。
- `[offset]` 是显式捕获列表。闭包**必须**显式捕获外层变量，否则报
  `variable ... must be explicitly captured`。
- `fn(T): U` 是函数类型，可作为参数类型。
- 闭包目前只在 LLVM 后端可用。

## 派生宏与属性

`#[no_main]` 是仓库里唯一在用的属性，用于标记「这是库文件，不需要 main」。
编译单文件库时会出现 `W4002 no 'main' function defined` 警告，属正常。

## 内置能力

- `print(a, b, c)`：语句形式，逗号分隔多个值。
- `clone(x)`：特殊调用，语义上复制，不走普通函数解析。
- 数组 `push(x)`、`length`：编译器内建 intrinsic，不是用户方法。
- `ref x` / `&x` 取地址，`@p` 解引用。

## 常见语法错误

| 症状 | 原因 |
| --- | --- |
| `expected '=>' for an expression body, got '->'` | 表达式体写成了 `->` |
| `expected expression after '=>'` | `=>` 后面没给表达式 |
| `boolean ops` 报错 | 用了 `&&` / `||`，应写 `and` / `or` |
| 闭包捕获报错 | 忘了写 `[captures]` |
| `unknown method 'x'` | 方法声明缺失，或 receiver 类型不匹配 |
