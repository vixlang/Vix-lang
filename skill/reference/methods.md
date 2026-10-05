---
name: vix-methods
description: Receiver methods in Vix, the three receiver kinds, resolution and ownership rules.
---

# 方法

完整设计说明见 [../../docs/METHODS.md](../../docs/METHODS.md)，这里是可操作摘要。

## 声明

方法在顶层声明，带 receiver，**没有 `impl` 块**：

```vix
type Point = struct {
    x: i32,
    y: i32
}

fn (Point) copy(): Point        { return self }               // owned，按值接收
fn (&Point) length(): i32       { return self.x + self.y }    // 共享借用
fn (&mut Point) shift(dx: i32)  { self.x = self.x + dx }      // 可变借用

fn main(): i32
{
    let mut p = Point{ x: 3, y: 4 }
    let moved = p.copy()
    p.shift(1)
    return moved.length() + p.length()
}
```

- 方法体里隐式存在 `self`，**不要**在参数列表里写 `self`。
- receiver 类型写在 `fn` 后面的括号里，借用标记写在类型前。
- 关联函数（不接收 receiver）就是普通函数，例如 `fn make_point(...)`；
  没有 `Point::new(...)` 这种调用形式。

## 三种 receiver

| 写法 | 语义 | 调用方要求 |
| --- | --- | --- |
| `(Point)` | 按值接收 | 非 Copy 值会被移动 |
| `(&Point)` | 共享借用 | 无 |
| `(&mut Point)` | 可变借用 | 接收者必须是 `let mut` |

对不可变值调用 `&mut` 方法会报 `E5006 shared reference is read-only`。

## 调用

```vix
let mut p = make_point(3, 4)
print(p.length())
p.shift(1)
```

按**静态 receiver 类型 + 方法名**解析，所以不同类型可以有同名方法：

```vix
type Point = struct { x: i32 }
type Vector = struct { n: i32 }

fn (&Point) size(): i32  { return self.x }
fn (&Vector) size(): i32 { return self.n }

fn main(): i32
{
    let p = Point{ x: 5 }
    let v = Vector{ n: 7 }
    return p.size() + v.size()   // 分别解析到 Point::size 和 Vector::size
}
```

## 编译器内部怎么看方法

如果你要改编译器，先读这一节，能省掉大量试错。

方法相关逻辑**集中**在四个文件，别在别处重建：

| 文件 | 职责 |
| --- | --- |
| `src/receiver.vix` | `ReceiverKind` ADT：`ReceiverNone/Owned/Shared/Mut` |
| `src/intrinsic.vix` | 运行时内建方法（数组 `push`）的注册表 |
| `src/method_decl.vix` | `MethodSignature` / `MethodTable` / `MethodResolution` 及访问器 |
| `src/method.vix` | `method_table_build`、`method_resolve`、`method_qualify` |

数据流：

1. parser 解析 receiver，写入 `AstFunction.receiver_kind` / `receiver_type`，
   并用 `method_qualify` 生成存储名 `Point::length`。
2. parser 出口 `method_program_attach` 建表，表挂在 `ExprStore.methods` 上。
   analyzer 不走 main.vix 管线，所以挂载点也在 parser 和 `ast_assemble`。
3. solver、ownership、MIR、LLVM 全部调用 `method_resolve(table, receiver_ty, name)`，
   拿到同一个 `MethodResolution`。
4. `analysis_lower_program_for_backend` 会把表里的类型串一起降级成
   `refptr:` / `ptr:` 形式。**漏掉这步会让借用 receiver 段错误**。

不要做的事：

- 不要用 `receiver + "::" + name` 拼方法名去查找。
- 不要把 receiver kind 当数字比较。
- 不要按方法名（如 `push`）判断是不是内建。
- 不要在调用方自己算 receiver 的参数下标。

这些都有门禁脚本兜底：

```sh
sh scripts/check-method-invariants.sh
```

改方法相关代码后必须跑它，跑完再跑 `python3 tests/methods_e2e.py`。
