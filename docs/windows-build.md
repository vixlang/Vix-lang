# Windows 平台构建指南(MSVC 工具链)

本分支(bootstrap)的编译器是自举式的:先用仓库里的种子 IR(`seed/vixc.ll`)
编译出种子编译器,再用它编译 `src/main.vix` 得到自举编译器。

Windows 上有两条构建路径:

1. **MSVC 工具链**(本文档)——使用 LLVM Windows 发行包 + Visual Studio,
   即本仓库 `scripts/build-windows-msvc.sh` 的路线,已在 Windows 10/11 上验证
   通过完整的两级自举。
2. **MSYS2 CLANG64**(CI 路线)——见 `.github/workflows/build.yml`,CI 只构建
   种子编译器。注意 MSYS2 的 LLVM 是 GNU(MinGW)ABI,与 MSVC 版 LLVM
   发行包不兼容,两者不要混用。

## 前置条件

- **LLVM 发行包**(本仓库使用 21.1.0):下载
  `clang+llvm-21.1.0-x86_64-pc-windows-msvc.tar.xz` 并解压,记下其根目录
  (包含 `bin`、`lib`、`include`)。
- **Visual Studio**(或 Build Tools),需要安装 "使用 C++ 的桌面开发" 工作负载
  (提供 MSVC 编译器与 CRT)。
- **Windows SDK**。
- Git Bash(脚本依赖 POSIX shell 与 `cygpath`)。

## 一键构建

编辑 `scripts/build-windows-msvc.sh` 顶部的三个路径,然后在 Git Bash 中运行:

```sh
scripts/build-windows-msvc.sh                # 只构建种子编译器 build/vixc.exe
scripts/build-windows-msvc.sh --bootstrap    # 继续自举两级(build/vixc-bootstrap.exe、build/vixc-full.exe)
scripts/build-windows-msvc.sh --smoke        # 附带编译并运行一个 hello world
```

脚本内部做的事与 Linux 的 `seed.sh` 一一对应:

| 步骤 | 说明 |
| --- | --- |
| 编译 `src/helper.c`、`src/runtime.c`、`lib/api.c` | C 桥接层(需要 LLVM 头文件) |
| 编译 `lib/llvm/{Llc,Linker,Passes}.cpp` | LLVM/lld C++ 桥接层(`-fno-rtti -fno-exceptions`) |
| `clang --target=x86_64-pc-windows-msvc -c seed/vixc.ll` | 种子 IR 编译为 COFF 目标 |
| 链接 `build/vixc.exe` | 静态链接全部 LLVM/lld 库,`/STACK:16777216` 增大栈 |
| (`--bootstrap`)用 `vixc.exe` 编译 `src/main.vix` 并链接 | 得到 `vixc-bootstrap.exe`,再自举一级得到 `vixc-full.exe` |

### 链接细节(遇到问题再看)

- 发行包**不带 `libxml2s.lib`**,而 `--libs all` 中的 `LLVMWindowsManifest.lib`
  依赖它,因此脚本从链接行剔除该库;lld 的 COFF 驱动所需的少量
  `windows_manifest` 符号由 `lib/llvm/Linker.cpp` 中的 stub 提供
  (`isAvailable() == false`,Vix 生成 PE 时从不请求 manifest)。
- `--system-libs` 输出的系统库(psapi、ole32 等)以 `-Wl,` 前缀透传给链接器。
- 生成 PE 时 lld-link 通过 **`%LIB%` 环境变量**查找 CRT 与 SDK 导入库
  (`msvcrt.lib`、`ucrt.lib`、`vcruntime.lib`、`kernel32.lib`),这一点与
  link.exe 一致。

## 运行编译器

`vixc.exe` 在**链接生成 exe** 时(而非 `--check`/`-obj`/`-ll` 时)需要 `%LIB%`
包含 MSVC 与 SDK 的库目录。PowerShell 示例:

```powershell
$env:LIB = @(
  'E:\Desktop\clang+llvm-21.1.0-x86_64-pc-windows-msvc\lib',
  'E:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\MSVC\14.51.36231\lib\x64',
  'D:\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64',
  'D:\Windows Kits\10\Lib\10.0.26100.0\um\x64'
) -join ';'
.\build\vixc.exe hello.vix -o hello.exe
.\hello.exe
```

注意:仓库自带的 `std/`、`core/` 标准库 IO 目前基于 Linux `syscall` 指令
(`core/io.vix`),在 Windows 程序里请用 `extern "C"` 声明 msvcrt 的函数,
例如:

```vix
extern "C"
{
    fn puts(s: string): i32
}

fn main(): i32
{
    puts("Hello from Vix on Windows!")
    return 0
}
```

## 已知限制

- `--backend=self` / `--backend=self-lir` 走 MIR→`nasm -f elf64` 的汇编路径,
  且调用约定按 SysV 生成,目前仅在 Linux 上可用(CI 同样只在 Windows 构建种子
  编译器:"Avoid self-hosting on this target until its aggregate ABI path is
  stable")。默认的 LLVM 后端在 Windows 上完整可用。
- `tests/syntax_tests.py` 的 self 后端用例因此会在 Windows 上失败;其余
  (`--check`、`--ast-json`、`--ownership-check`、llvm 后端)可用。
- 编译器本体未链接 Boehm GC(Windows 的 lld-link 不支持 `--wrap`),长时间
  自举编译会多占一些内存,功能不受影响。
- 测试需要 LLVM 工具(如 `llvm-as`)时请将其 `bin` 目录加入 `PATH`;
  `tests/entry_alloca.py` 在缺少时会跳过 LLVM verifier 步骤。
