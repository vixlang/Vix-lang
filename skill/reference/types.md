---
name: vix-types
description: Vix structs, ADTs, Option, generics, references and pointers.
---

# 类型

## struct

```vix
type Point = struct {
    x: i32,
    y: i32
}

let p = Point{ x: 3, y: 4 }
print(p.x)
```

- 字段用逗号分隔，最后一个字段后面可以不带逗号。
- 字面量可以跨行。
- struct 是值类型，赋值与传参默认按值。

## ADT（代数数据类型）

无载荷：

```vix
type Color = Red | Green | Blue
```

带载荷：

```vix
type NumberResult = Valid(i32) | Invalid(string)
```

泛型 ADT，注意形参用 `:[...]`：

```vix
type Result:[T, E] = Ok(T) | Err(E)
type Box:[T] = Boxed(T)
```

构造与匹配：

```vix
let ok = Ok(42) : Result:[i32, string]

match ok
{
    Ok(v) -> print(v)
    Err(e) -> print(e)
}
```

ADT 可以比较相等：`if (a == b) { ... }`，未带载荷的变体（如 `Empty`）可直接比较。

## Option

内置 `Option[T]`，类型语法糖 `?T`：

```vix
fn first_or_none(list: [string]): ?string
{
    if (list.length > 0) { return Some(list[0]) }
    return None
}
```

## 泛型

函数泛型，形参声明用 `:[T]`，调用时显式给实参 `[i32]`：

```vix
fn id:[T](value: T): T
{
    return value
}

let number = id[i32](42)
let flag = id[bool](true)
```

多参数：

```vix
fn apply:[T, U](value: T, transform: fn(T): U): U
{
    return transform(value)
}
```

嵌套泛型实参写作 `Box:[?T]`。

编译器对泛型函数做单态化（`generic_monomorphize_program`），
所以泛型不是运行时多态。

## 数组与字符串

```vix
let arr = [1, 2, 3]
arr[0] = 10
let n = arr.length
arr.push(4)

let names: [string] = ["a", "b"]
let text = "hello" + " " + "world"
```

- 数组类型写作 `[T]`，字面量写作 `[a, b, c]`。
- 空数组字面量 `[]` 需要上下文推断元素类型。
- `length` 是字段式访问，不是方法调用。
- 字符串用 `+` 拼接。

## 引用与指针

```vix
fn deref:[T](value: &T): T
{
    return @value
}

fn main(): i32
{
    let x = 10
    let p = ref x        // 取地址，等价写法 &x
    print(@p)            // 解引用
    let q: ref i32 = nil // 可空引用
    if (q == nil) { print("null") }
    return 0
}
```

- 类型位置写 `&T` 或 `ref T`。
- 取地址写 `ref x` 或 `&x`。
- 解引用写 `@p`。
- `nil` 是空指针字面量。

## 所有权（实验性）

用 `--ownership-check` 才会跑：

```sh
build/vixc-patched prog.vix --ownership-check
```

已实现：借用期间不能移动（E5001/E5002）、`&mut` 接收者要求可变值（E5006）。
**未实现**：按值传递 struct 参数的 use-after-move 诊断。普通函数和
owned receiver 方法行为一致，不要假设它比语言其他部分更严格。

## 类型错误的读法

`E3002 type mismatch: expected X, got Y` 表示两种类型不兼容。
Vix **不做隐式转换**，需要显式构造或改写表达式。

用 `--explan=E3002` 可以看该错误码的完整解释：

```sh
build/vixc-patched --explan=E3002
```
