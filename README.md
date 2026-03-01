![Vix logo](images/README/1770378110202.png)

# Vix 编程语言

[![自举进度](https://img.shields.io/badge/自举-90%25-orange)]()
[![后端](https://img.shields.io/badge/后端-LLVM%20%7C%20QBE%20%7C%20C++-brightgreen)]()
[![VS Code扩展](https://img.shields.io/badge/VS%20Code-扩展-purple)]()
[![许可证](https://img.shields.io/badge/许可证-MIT-green)](LICENSE)

Vix 是一种轻量级编译型语言，旨在提供**接近原生 C++ 的执行速度**，同时保持脚本语言的简洁性和易用性。

[🌟 快速开始](#-快速开始) | [📖 文档](Docs/README.md) | [🧩 VS Code扩展](#) | [🤝 参与贡献](#参与贡献)

## ✨ 核心亮点

### ⚙️ 三后端架构（业内罕见）

Vix 是目前极少数**同时支持三个后端**的编译型语言，你可以根据场景自由选择：


| 后端     | 特点                        | 适合场景                     |
| -------- | --------------------------- | ---------------------------- |
| **LLVM** | 工业级优化，代码质量高      | 生产环境、性能敏感应用       |
| **QBE**  | 极简轻量，编译速度快        | 开发调试、学习研究           |
| **C++**  | 直接生成 C++ 代码，可读性强 | 需要人工干预、移植到特殊环境 |

> 同时实现 LLVM、QBE 和 C++ 三个后端，是 Vix 最硬核的技术特色之一。

### 🚀 即将自举

编译器的下一个版本将能用自己编译自己——这是编程语言走向成熟的重要里程碑。

### 🧩 VS Code 扩展

提供语法高亮和错误提示，开箱即用，让开发体验更顺畅。

### 🌐 跨平台 + 多架构

- **操作系统**：Windows、Linux、macOS
- **处理器架构**：x86、ARM、RISC-V

## 🚀 快速开始

### 1. 安装依赖

```shell

apt install gcc g++ flex bison llvm clang-18 # Ubuntu/Debian

yum install gcc gcc-c++ flex bison llvm clang-18 # CentOS/RHEL

brew install flex bison llvm clang-18 # macOS

pacman -S flex bison g++ gcc llvm clang-18 # Arch Linux
```

### 2. 编译 Vix

```shell
make
```

### 3. 验证安装

```shell
vixc -v
```

### 4. 第一个 Vix 程序

创建一个 `hello.vix` 文件：

```vix
fn main() -> i32 {
    print("Hello, Vix!")
}
```

编译并运行：

```shell
vixc hello.vix -o hello
./hello
```

## 📚 示例预览

这里有几个简单的例子，让你快速感受 Vix 的语法：

### 斐波那契数列

```vix
fn fib(n: i32) -> i32 {
    if (n <= 1) {
        return n
    }
    return fib(n-1) + fib(n-2)
}

fn main() -> i32 {
    print(fib(10))
}
```

### 变量和循环

```vix
fn main() -> i32 {
    sum = 0
    for (i in 1 .. 100) {
        sum = sum + i
    }
    print("1 到 100 的和：", sum)
}
```

更多示例请查看 [examples](examples) 目录。

## 📖 文档

- [语言参考](Docs/README.md) - 语法、类型系统、内置函数等
- [编译器使用指南](Docs/compiler.md) - 命令行选项、配置等
- [后端对比](Docs/backends.md) - LLVM/QBE/C++ 后端的详细对比
- [常见问题](Docs/faq.md)

## 🧩 VS Code 扩展

为了让 Vix 的开发体验更友好，我们提供了官方的 VS Code 扩展：

- ✅ 语法高亮
- ✅ 错误提示
- ✅ 代码片段
- ✅ 编译任务集成

## 🤝 参与贡献

我们欢迎各种形式的贡献！包括但不限于：

- 💡 提出语法建议
- 📝 撰写文档
- 🐛 报告 bug
- 🔧 提交代码
- 📦 开发包管理工具（VPM）
- 📚 完善标准库

请阅读[贡献指南](Docs/CONTRIBUTING.md)开始。

## 🗺️ 项目生态

Vix 正在逐步构建自己的生态：


| 项目             | 描述                            | 状态             |
| ---------------- | ------------------------------- | ---------------- |
| **Vix 编译器**   | 核心编译器（LLVM/QBE/C++ 后端） | 开发中，即将自举 |
| **VPM**          | Vix 包管理器                    | 社区贡献中       |
| **标准库**       | 常用数据结构和函数              | 社区贡献中       |
| **VS Code 扩展** | 编辑器支持                      | 已发布           |

## 📄 许可证

本项目基于 MIT 许可证开源 - 查看 [LICENSE](LICENSE) 文件了解详情。

## 📬 联系方式

- 邮箱：[popolk1871@outlook.com](mailto:popolk1871@outlook.com)
- GitHub Issues：直接在本仓库提交
- 💬 [QQ群：130577506](https://qm.qq.com/cgi-bin/qm/qr?k=130577506)（加入一起聊 Vix）

**如果你对 Vix 感兴趣，欢迎 star、fork、提 issue，或者直接上手试试！**

> 别犹豫！就现在!
