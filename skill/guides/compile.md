---
name: vix-compile
description: How to build the Vix compiler, compile Vix programs, and run them on macOS.
---

# 编译

## 编译一个 Vix 程序

```sh
# 只做语法与类型检查（最快，改完必跑）
build/vixc-patched prog.vix --check

# 只看所有权
build/vixc-patched prog.vix --ownership-check

# 输出 LLVM IR / 汇编 / 目标文件
build/vixc-patched prog.vix -ll -o prog.ll
build/vixc-patched prog.vix -S  -o prog.s
build/vixc-patched prog.vix -obj -o prog.o
```

## 在 macOS 上跑起来

内置链接器只实现了 ELF/COFF，`vixc prog.vix -o prog` 在 macOS 上**无法链接**。
self/LIR 后端又拒绝非 x86_64 主机。所以走 LLVM 后端出 Mach-O 目标文件，
再用平台工具链链接：

```sh
sh scripts/run-vix.sh examples/impl2.vix
VIX_ARGS="-opt=l2" sh scripts/run-vix.sh prog.vix
```

编辑器里让 Build & Run 面板和 `vix.run` 也能用，把编译器指到 shim：

```json
{ "vix.compilerPath": "/绝对路径/Vix-lang/scripts/vixc-macos.sh" }
```

shim 会拦截「生成可执行文件」这一种调用，其余模式原样转发给真正的 vixc，
所以诊断和 LSP 不受影响。

## 构建编译器本身

```sh
# 只要对象文件，验证自举（最快）
build/vixc src/main.vix -obj -o /tmp/vixc.o

# 完整：重建支持对象 + patched vixc + analyzer
sh scripts/build-analyzer.sh
```

`build/vixc` 是**已有的引导编译器**，用它编译 `src/` 源码；
`build/vixc-patched` 是用当前源码编出来的新编译器，跑测试用它。

改 `src/` 后的最短验证链：

```sh
build/vixc src/main.vix -obj -o /tmp/vixc.o && echo self-host ok
sh scripts/build-analyzer.sh
```

## CLI 速查

```text
-o <file>            输出文件
-ll / -S / -obj      发射 LLVM IR / 汇编 / 目标文件
-opt=lN              优化级别 0..3
--target <triple|boot16>
--backend <llvm|self|self-opt|self-lir>   默认 llvm
-l<lib> -L<dir>      链接库与搜索路径
--check              只检查
--ownership-check    所有权检查（实验性）
--lex / --parser / --ast / --ast-json     打印词法/语法/AST
--typeinfer / --semantic                  打印类型/语义结果
--debug=<typed-mir|mir|lir|asm|llvm>      打印中间产物
--module-graph       打印模块依赖图
--explan=E3001       解释错误码
--diag-format=<human|short|json>
--color=<auto|always|never>
```

## 多文件

`mod` 会触发模块图构建与合并；`--module-graph` 可以先看依赖关系。
编译器的聚合入口是 `src/main.vix`（内部 `include "sys.vix"` 指向 `src/sys.vix`）。

## 格式化

```sh
sh scripts/vix-fmt.sh file.vix          # 就地格式化
sh scripts/vix-fmt.sh src/              # 整个目录
sh scripts/vix-fmt.sh --check src/      # CI：只报告，退出码非 0 表示需要格式化

build/vixc-patched file.vix --fmt -o -             # 输出到 stdout
build/vixc-patched file.vix --fmt --max-width=80   # 行宽
build/vixc-patched file.vix --fmt --indent=2       # 缩进
build/vixc-patched file.vix --fmt --use-tabs
```

配置写在源文件旁边的 `vix-fmt.toml`：`max_width`、`indent_width`、`use_tabs`。
命令行参数优先。编辑器里走同一个实现，缩进取 VS Code 的 `editor.tabSize`。

写代码时**不要手工对齐空格**：格式化器会把多余空格折叠成一个。
详细规则见 [../../docs/FORMATTING.md](../../docs/FORMATTING.md)。

## 常见编译失败

| 现象 | 处理 |
| --- | --- |
| `undefined function 'x'`，但函数确实存在 | 该文件单独 `--check` 缺依赖；用聚合入口验证，或补 import |
| `duplicate struct 'X'` | 同一文件被两种路径拼写包含；统一 import 拼写 |
| `self backend only supports x86_64 targets` | 换 LLVM 后端 |
| 链接期 `ld: -lto_library ...` | 用 `clang++` 而不是 `clang` 链接 |
| LLVM IR `expected value token` | 后端 codegen 生成了非法 IR，是真 bug，要定位 |
