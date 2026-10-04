# Vix Analyzer for VS Code

Vix 语言支持：语法高亮、Vix Aurora Dark 主题、语言服务器（诊断 / 悬停 / 跳转定义 /
引用 / 重命名 / 格式化）以及编译工作流。

## Vix 面板

点活动栏的 Vix 图标打开面板。面板里有**两个可执行文件**需要配置：

| 字段 | 用途 | 默认 |
|---|---|---|
| **Compiler (vixc)** | Run / Build / Check / Assembly / Object，以及编译器诊断 | PATH 上的 `vixc` |
| **Language server (vix-analyzer)** | 诊断、悬停、跳转、重命名、格式化 | 本扩展内置的副本 |

两个字段**都可以直接编辑**（粘贴路径后按 Enter 或失焦即生效）。每个字段旁边：

- **Browse** —— 用文件选择器挑可执行文件
- **Auto-detect** —— 编译器缺失时出现，依次尝试 PATH 上的 `vixc`、
  `<工作区>/build/vixc-patched`、`<工作区>/build/vixc`、`/usr/local/bin/vixc`、
  `/opt/homebrew/bin/vixc`
- **Use bundled server** —— 把语言服务器改回本扩展内置的副本

圆点是红的就是那个可执行文件起不来，面板会直接给出下一步该点哪里。
语言服务器路径一旦修改会自动重启，不需要重载窗口。

## 命令

`Vix: Select Compiler Executable (vixc)`、`Vix: Select Language Server Executable`、
`Vix: Run`、`Vix: Check`、`Vix: Compile`、`Vix: Restart Language Server`、
`Vix: Format Document`、`Vix: Focus Panel`。

## 设置

- `vix.compilerPath` —— vixc 路径（与面板字段同一个值）
- `vix.autoCompile` / `vix.autoCompileDelay` —— 编辑后刷新编译器诊断
- `vix.compilerErrorFormat` —— `bootstrap` / `c-cpp` / `none`
- `vixAnalyzer.serverPath` —— 覆盖内置语言服务器；留空用内置的

## 从源码构建

    sh scripts/build-analyzer.sh
    cd editors/vscode
    cp ../../build/vix-analyzer server/vix-analyzer
    npm install && npm run package
