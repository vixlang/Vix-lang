<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="images/README/image.png">
    <source media="(prefers-color-scheme: light)" srcset="images/README/image.png">
    <img
      alt="Vix Programming Language"
      src="images/README/image.png"
      width="50%">
  </picture>

[Website][Vix] | [Getting Started][Getting Started] | [Learn][Learn] | [Documentation][Documentation] | [Contributing][Contributing]
</div>
This is the main source repository for Vix It contains the compiler,
runtime-related components, examples, and language documentation.

[Vix]: https://vixlang.github.io
[Getting Started]: Docs/en/getting-started.md
[Learn]: Docs/en/what-is-vix.md
[Documentation]: Docs/en/syntax.md
[Contributing]: https://github.com/vixlang/Vix-lang/issues


> 🎉 🎉 🎉 Vix is fully self-hosted! The compiler now compiles itself. No external dependencies. Next: stdlib, packages, and tooling 🚀



> This branch is an experimental bootstrap branch, and it's not recommended to download it for now. If you want to use vix, please go to: https://github.com/vixlang/Vix-lang


## Why Vix?

- **Performance:** Vix compiles to native code with an LLVM-based backend and is designed for low-overhead execution.

- **Reliability:** Static typing and compile-time checks catch common errors earlier.

- **Simplicity:** The language keeps syntax concise while still supporting practical features like functions, modules, structs, pointers, generics, and control flow.

## Hello world

``` vix
import "std/io.vix"
fn main(): i32
{
    puts("Hello,world!")
    return 0
}
```

## Running a program

On Linux, `vixc prog.vix -o prog && ./prog` works directly. On macOS vixc's
embedded linker only implements the ELF and COFF drivers, so the LLVM backend
emits a Mach-O object and the platform toolchain links it:

```sh
sh scripts/run-vix.sh examples/impl2.vix
```

The self/LIR backend refuses non-x86_64 hosts, which is why the LLVM backend is
the one used above.

### From the editor

The Vix Build & Run panel and the `vix.run` command invoke
`vixc ... -o <name> <file>`, which cannot link on macOS. Point the extension at
the shim to make those work:

```json
{
  "vix.compilerPath": "/absolute/path/to/Vix-lang/scripts/vixc-macos.sh"
}
```

The shim forwards every other mode (`-obj`, `-S`, `--check`, ...) to the real
compiler unchanged, so the problems panel and the language server are unaffected.

## Documentation

- LearnVix:[GitHub Link](https://github.com/vixlang/LearnVix)
- VixDocs:[GitHub Link](https://github.com/vixlang/LearnVix)
- VixDocs for ZRead:[Link](https://zread.ai/vixlang/Vix-lang)

## Getting Help

- Open a discussion in [GitHub Issues](https://github.com/Daweidie/vix-lang/issues).
- Contact: [popolk1871@outlook.com](mailto:popolk1871@outlook.com)
- QQ Group: 130577506

## Contributing

Contributions are welcome, including language design feedback, bug reports,
tests, standard library improvements, and documentation updates.

To start contributing, please open or pick an issue:
[vix-lang issues](https://github.com/Daweidie/vix-lang/issues)

## License

Vix is distributed under the Apache License 2.0.
See [LICENSE](LICENSE) for details.

## Ecosystem

| Project               | Description                                            | Status                                                           |
| -----------------------| --------------------------------------------------------| ------------------------------------------------------------------|
| **Vix Compiler**      | Core compiler with LLVM-focused backend implementation | In active development                                            |
| **Very**              | Package manager for Vix                                | Community contribution [Very](https://github.com/vixlang/Very)   |
| **Standard Library**  | Common APIs and utilities                              | Community contribution                                           |
| **VS Code Extension** | Editor support for Vix                                 | Published [Link](https://github.com/vixlang/ext-VixLangAnalyzer) |

