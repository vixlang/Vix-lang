---
name: vix-debug
description: Reading Vix diagnostics, inspecting intermediate output, and debugging the compiler.
---

# 调试

## 诊断格式

```
error[E3002]: type mismatch
  in function main: type mismatch: expected Point, got i32
 --> prog.vix:12:9
   |
11 |     let p = 1
12 |     return p.sum()
   |            ^ ...
   = note: Vix does not implicitly convert between unrelated value types
   = help: make the value type match the expected type, or change the declaration
```

每段：`error[CODE]: title` → 函数上下文 → `--> 文件:行:列` → 源码片段 → note → help。

错误码分段：`E1xxx` 解析、`E2xxx` 命名、`E3xxx` 类型、`E5xxx` 所有权。

```sh
build/vixc-patched --explan=E5006   # 看错误码完整解释
build/vixc-patched prog.vix --diag-format=json
```

编辑器的问题面板解析的是 bootstrap 格式，正则要求 `error[CODE]:` 开头，
所以链接器的 `ld: warning:` 之类不会被误报成错误。

## 看中间产物

按管线顺序逐层打印，定位问题在哪一层：

```sh
build/vixc-patched prog.vix --lex            # 词法
build/vixc-patched prog.vix --parser         # 语法树
build/vixc-patched prog.vix --ast            # AST
build/vixc-patched prog.vix --ast-json
build/vixc-patched prog.vix --typeinfer      # 类型推断
build/vixc-patched prog.vix --semantic       # 语义
build/vixc-patched prog.vix --ownership-check
build/vixc-patched prog.vix --debug=typed-mir
build/vixc-patched prog.vix --debug=mir
build/vixc-patched prog.vix --debug=lir
build/vixc-patched prog.vix --debug=asm
build/vixc-patched prog.vix --debug=llvm
build/vixc-patched prog.vix --module-graph
```

## 定位后端 bug

LLVM IR 非法时编译器会打印出错的行号和 IR 文本：

```
vixc: /tmp/xxx.raw.ll:97162:20: error: expected type
  %load44232 = load, align 4
```

用 `-ll -o /tmp/out.ll` 拿到完整 IR，对照出错行号找生成点。
常见根因：

| IR 症状 | 根因 |
| --- | --- |
| `load, align 4`（缺类型） | 类型串没解析出来，返回了空串或 `unknown` |
| `store %Struct %x, <null operand!>` | 聚合类型赋给了全局变量；Vix 全局变量对复杂聚合支持有限 |
| `add %Struct %x, i32 1` | Option 聚合值参与整数运算 |

## 运行期崩溃

```sh
build/vixc-patched prog.vix -obj -o /tmp/prog.o
clang++ /tmp/prog.o runtime/runtime.o -o /tmp/prog
lldb /tmp/prog          # 或直接跑，看退出码
```

退出码 `-11`（SIGSEGV）通常是 ABI 不匹配：receiver 类型串没随 lowering 转换、
函数签名注册错、或参数按值/按引用不一致。

**改方法相关代码后如果借用 receiver 段错误，先检查
`analysis_lower_program_for_backend` 有没有降级 method table 里的类型串。**

## 改编译器时的自检顺序

1. `build/vixc --check <改动的文件>`（能单文件检查的话）
2. `build/vixc src/main.vix -obj -o /tmp/x.o` 自举
3. `sh scripts/build-analyzer.sh` analyzer 也能构建
4. 相关门禁脚本 + 端到端脚本
5. 编译差分回归

## 读错误信息的技巧

- `undefined function` 出现在**聚合入口**（`src/main.vix`）时，
  往往说明某个 `mod` 的文件**整体解析失败被跳过**，
  而不是那个函数真丢了。去单独检查那个文件，找真正的语法错误。
- `duplicate struct` 说明同一文件被两种路径包含，统一 import 拼写。
- 单文件 `--check` 报缺符号但聚合能过，通常是 import 依赖问题，不是 bug。
