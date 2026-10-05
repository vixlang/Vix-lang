---
name: vix-ffi-runtime
description: Calling C from Vix, memory, strings and the runtime library.
---

# FFI 与运行时

## extern "C"

```vix
extern "C"
{
    fn puts(value: ptr): i32
    fn printf(fmt: string, ...): i32
    fn malloc(size: usize): string
    fn free(p: ptr): void
}
```

- 变参用 `...`，最后一个具名参数之后再写。
- 声明可以写在文件任意顶层位置，不必和调用点同文件。
- 链接系统库用编译器参数 `-l<lib>` / `-L<dir>`。

## 常用类型

| 类型 | 说明 |
| --- | --- |
| `i8 i16 i32 i64` | 有符号整数 |
| `u8 u16 u32 u64` | 无符号整数 |
| `usize isize` | 指针宽度整数 |
| `f32 f64` | 浮点 |
| `bool` | 布尔 |
| `char` | 字符 |
| `string` | 字符串（可直接传给 C 的 char*） |
| `ptr` | 无类型指针 |

## 字符串操作

语言层只有拼接和索引。需要逐字节处理时用运行时/FFI：

```vix
extern "C"
{
    fn compiler_string_byte(text: string, index: i32): i32
}
```

编译器自身大量使用这个函数做字节级判断（`src/builder.vix` 里有
`type_text_slice`、`type_text_trim` 等现成工具，改编译器时优先复用）。

手工拼字符串的惯用写法是分配 2 字节小串再拼接：

```vix
extern "C" { fn malloc(size: usize): string }

pub fn char_string(byte: i32): string
{
    let text: string = malloc(2)
    text[0] = byte
    text[1] = 0
    return text
}
```

## 运行时库

`src/runtime.c` 提供数组与安全算术，编译时随程序一起链接
（`runtime/runtime.o`）：

| 函数 | 用途 |
| --- | --- |
| `vix_array_len` | 数组长度 |
| `vix_array_push_i32` / `vix_array_push_ptr` / `vix_array_push_bytes` | 数组 push 的三种元素形态 |
| `vix_array_slice_bytes` | 切片 |
| `vix_string_concat` | 字符串拼接 |
| `vix_string_slice` | 字符串切片 |
| `vix_safe_sdiv_i32` / `vix_safe_srem_i32` | 除零安全的整除/取余 |

数组的 `push` 在语言层是 intrinsic，由 `src/intrinsic.vix` 注册，
LLVM 后端按元素类型分派到上面三个 push 函数之一。

## 编译器插件接口

`lib/api.c` 导出给宿主进程调用的接口，例如 `vix_api_llc_compile_object`、
`vix_api_link_executable`。用 C/C++ 嵌入 vixc 时看这里。

## 链接

- Linux：`vixc prog.vix -o prog` 可直接产出可执行文件（内置 ELF 链接器）。
- macOS：内置链接器只有 ELF/COFF 驱动，必须走 LLVM 后端 + 平台链接器，
  用 [../../scripts/run-vix.sh](../../scripts/run-vix.sh)，
  细节见 [../guides/compile.md](../guides/compile.md)。
- 生成的符号会被 `mangle_flat_name` 处理，形如 `__V5Point6length`
  （长度前缀编码），不要手写这些符号名。
