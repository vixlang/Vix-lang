# vix-analyzer

独立于 vixc 的 Vix 编辑器分析器（LSP 服务器）。关系类似 rustc 与 rust-analyzer：
复用 lexer/parser/AST/sema，但不调用 vixc driver，也不解析 vixc 的命令行输出。

## 增量

分析分三层，每层都能独立验证：

**1. 存根树（语法层）** —— 编辑后只重扫受影响的那一个顶层声明。被替换的子树留在
arena 里标记 `alive = 0`（墓碑），未受影响的声明保持完全相同的 `StubNodeId`。

**2. AST 片段拼装** —— 每个顶层声明单独解析成一个片段，程序由片段按源序拼装而成。
片段按**文本内容**复用：内容没变的声明直接沿用已解析的 AST，只按行差平移位置；内容变了
的才重新解析。索引重映射按标签区分字段语义（`assign_idx` 在 `if` 上是 else **语句**
索引、其余标签是表达式；`direct_ret` 在 `store_index` 上是索引、在 `return` 上是字面量），
否则会把语句图接错并让语义检查无限递归。

片段行号必须对齐编译器的 **combined source**：每个引号形式的 `import "..."` 行会被丢弃，
其后的行号整体减一。片段解析时用 `物理行 − 该行之前的引号 import 数` 作为 token 偏移。

**3. 语义检查的函数级缓存** —— 只有当「程序对外表面」（所有函数签名 + struct/adt/global）
指纹不变时，才按函数复用缓存的诊断，只重查改动的那一个函数体；表面一变就整体失效。
函数体的键是语义哈希（不含行列），所以纯空白编辑也能命中缓存。

实测（2 个函数，编辑 `main` 函数体）：

    {"method":"vix/analysisStats",
     "params":{"reusedNodes":2,"freshNodes":2,"aliveNodes":3,"fullFile":0,
               "reusedFunctions":1,"checkedFunctions":1}}

`reusedNodes`/`freshNodes` 是存根树节点，`reusedFunctions`/`checkedFunctions` 是语义检查。
新增函数这类改变表面的编辑会让缓存整体失效（`reusedFunctions:0, checkedFunctions:2`）。

## 正确性验证

```
python3 vix-analyzer/tests/lsp_differential.py ./build/vix-analyzer 200
```

同一文件、同一串编辑，分别用「默认路径」和「每次全量重建」跑，逐字节比较诊断与符号：

    compared=102 mismatches=0

三条路径（片段+函数缓存 / 存根树+整篇解析 / 全量重建）互相对比均为 0 差异。

## 体验

    sh scripts/build-analyzer.sh
    ./build/vix-analyzer                     # 通过 stdio 讲 LSP
    python3 vix-analyzer/tests/lsp_session.py ./build/vix-analyzer

实测输出：

    documentSymbol  add   selectionRange 0:3  range -> 3:1
                    main  selectionRange 5:3  range -> 9:1
    hover           add: fn(a: i32, b: i32) -> i32
    definition      file:///tmp/demo.vix 0:3-0:6
    references      2（声明处 + 使用处）
    rename          2 处编辑
    语法错误        E1002 unexpected token ':'  0:10-0:11
    语义错误        E2001 undefined symbol 'missing'  8:11-8:12

## 格式化

`textDocument/formatting` 由 analyzer 提供，扩展的 `vix.formatDocument` 与 VS Code 内置的
Format Document 都走它。

**实现位置**：格式化核心在 `src/fmt.vix`，与 `vixc --fmt` **共用同一份代码**，
所以编辑器和命令行不可能给出不同结果。analyzer 只负责把 LSP 请求翻译成配置：

- 缩进宽度/制表符来自请求里的 `options.tabSize` / `options.insertSpaces`
  （也就是 VS Code 的 `editor.tabSize` / `editor.insertSpaces`）
- 行宽来自源文件旁边的 `vix-fmt.toml`

**取舍说明**：AST 不保留注释，所以"从 AST 重新打印整个程序"会把注释全部删掉。因此格式化器
只用 parser 判断文件能否解析，实际工作在原始字节上做。

完整规则与配置见 [../docs/FORMATTING.md](../docs/FORMATTING.md)。

安全性质（都有测试）：

- initialize 结果声明了 `documentFormattingProvider`，否则编辑器根本不会发请求
- 语法错误的文件**返回空编辑**，不动它（避免把半成品按猜测重排）
- **幂等**：格式化两次，第二次返回 0 个编辑
- 注释、字符串字面量逐字节保留

    python3 vix-analyzer/tests/lsp_formatting.py ./build/vix-analyzer
    python3 vix-analyzer/tests/lsp_format.py ./build/vix-analyzer
    python3 vix-analyzer/tests/lsp_format_safety.py ./build/vix-analyzer

**未做**：长行折行、`:` 对齐、运算符两侧空格归一化、导入排序。这些都要求真正从 AST
重新打印，而目前的 AST 会丢注释，所以先不做。

## 已实现

- stdio Content-Length 帧读写
- 完整递归 JSON 解析（对象/数组/转义/\uXXXX/代理对），节点带源码区间
- didOpen / didChange（含 range 增量编辑）/ didClose
- publishDiagnostics：parser 与 semantic 诊断转 LSP Diagnostic
- documentSymbol / hover / definition / references / completion / rename
- StubTree：arena + 墓碑 + 声明级 subtree replacement
- 符号索引：声明、按名解析、使用处引用
- 复用 src/lexer.vix、src/parser.vix、src/semantic.vix、src/diag.vix

## 模块

    ids.vix                 FileId / Revision / StubNodeId / SymbolId
    position.vix            byte range 与 LSP position/LineIndex 互转
    source_text.vix         文本与增量编辑
    source_db.vix           文件 overlay 与 revision
    stub_tree.vix           存根树、墓碑与声明级拼接
    incremental.vix         重解析范围选择
    json.vix                JSON 解析与编码
    analysis.vix            单文档分析（增量语法 + 全量语义）
    workspace.vix           文档集合
    queries.vix             hover / definition / references / completion / rename
    symbol_index.vix        符号与引用索引
                           （格式化实现已移到 src/fmt.vix，与 CLI 共用）
    diagnostics.vix         诊断转换与序列化
    lsp_protocol.vix        LSP 编解码
    lsp_transport.vix       stdio 帧
    lsp_handlers.vix        LSP 方法与参数
    lsp_server.vix          分发循环
    support.c               前端符号垫片与 stdin/stdout 桥接

## 构建说明

vixc 内嵌链接器只有 ELF 和 COFF 驱动，没有 Mach-O 驱动，所以 macOS 上
`vixc prog.vix -o prog` 无法链接（examples/hello.vix 同样失败）。LLVM 后端本身正常，
能产出合法 Mach-O 目标文件，因此构建方式是「LLVM 后端出目标文件 + 平台工具链接」。

编译器还需要 src/codegen.vix 里的 Option 聚合体修复：原先 `Some(struct)` 会生成
`%add = add %P %insert, i32 1` 这种非法 IR。修复后聚合载荷改为 alloca + store 传递
指针，解码侧对应 load。scripts/build-analyzer.sh 会先用现有 build/vixc 编译出带修复的
build/vixc-patched，再用它构建 analyzer。

工具链要求（可用环境变量覆盖）：LLVM 21 头文件与 llvm-config、lld 头文件与
lld/LLVM 库、bdw-gc。

## 已知限制

- textDocumentSync 声明为 Full；range 增量编辑已支持，语法层是增量的，语义层不是
- references 与 rename 是单文件、基于标识符名的解析，尚未做作用域与遮蔽分析
- 跨文件 module graph 与依赖失效尚未接入 analyzer
- 局部变量没有进入 symbol index，因此 hover 不覆盖局部变量
- 格式化只处理缩进与空行，不做折行/对齐/空格归一化
- 未实现 semanticTokens、codeAction