---
name: vix-test
description: Test entry points in the Vix repository and how to add a regression.
---

# 测试

## 入口

| 命令 | 作用 |
| --- | --- |
| `python3 tests/run.py` | 数值语料回归（`tests/files/test*.vix`），比对退出码 |
| `python3 tests/run.py --self` | 用 self 后端跑；**arm64 主机不可用** |
| `python3 tests/smoke.py` | 400 个 smoke + 400 个宏 smoke |
| `python3 tests/syntax_tests.py` | `tests/syntax/` 语法与 AST 断言 |
| `python3 tests/complex_tests.py` | 组合场景 |
| `python3 tests/diagnostics.py` | 诊断快照 |
| `python3 tests/fuzz.py` | 模糊测试 |
| `python3 tests/stree.py` | 语法树 |
| `sh scripts/check-method-invariants.sh` | 方法设计不变量门禁 |
| `python3 tests/methods_e2e.py` | 方法端到端（LLVM 后端编译+链接+运行） |
| `python3 vix-analyzer/tests/lsp_methods.py build/vix-analyzer` | LSP 方法解析与误报检查 |
| `python3 vix-analyzer/tests/lsp_format_safety.py build/vix-analyzer` | 格式化安全性 |
| `python3 editors/vscode/tests/grammar_test.py` | 编辑器语法高亮正则 |

注意 `tests/run.py` 依赖可执行产物，arm64 macOS 上 self 后端不可用，
LLVM 后端需要先链接。**在本机更可靠的回归方式是编译差分**（下一节）。

## 编译差分（本机最实用的回归手段）

拿改动前后的两个编译器，对全量测试文件跑同一条命令，比对结果：

```sh
# 旧编译器先留一份
cp build/vixc-patched /tmp/vixc-before

# 改完重建后
same=0; diff=0
for f in tests/*.vix tests/files/*.vix examples/*.vix; do
  /tmp/vixc-before "$f" -obj -o /tmp/a.o >/dev/null 2>&1; a=$?
  build/vixc-patched "$f" -obj -o /tmp/b.o >/dev/null 2>&1; b=$?
  if [ "$a" = "$b" ]; then same=$((same+1)); else diff=$((diff+1)); echo "$f old=$a new=$b"; fi
done
echo "same=$same differing=$diff"
```

预期：`differing` 只包含**你有意改进**的文件，且应当是 `old=1 new=0` 方向。
出现 `old=0 new=1` 就是回归，必须查清。

## 加一个回归用例

放 `tests/<feature>_*.vix`，用退出码表达结果，不要依赖 stdout 比对（除非确实要测输出）。
只有断言不成立时才返回非 0：

```vix
type Point = struct {
    x: i32,
    y: i32
}

fn (&Point) sum(): i32
{
    return self.x + self.y
}

fn main(): i32
{
    // struct 字面量必须写全所有字段
    let p = Point{ x: 20, y: 22 }
    if (p.sum() != 42) { return 1 }
    return 0
}
```

负例（应当编译失败）单独放一个文件，并在测试脚本里断言错误消息片段。
命名用 `*_negative_*.vix` 表明预期失败。

端到端用例加到 `tests/methods_e2e.py` 那种表格驱动的脚本里，
ACCEPT 表写「文件名 + 期望退出码」，REJECT 表写「文件名 + 期望错误片段」。

## 断言纪律

- 断言要能在**修复前失败**，否则它没有价值。
  写完新测试先确认它在旧代码上会失败。
- 不要写「只要不崩就算过」的测试。
- 不要把已知不支持的场景写成通过的测试而不加注释——
  注释里写清楚为什么它现在只能这么断言。
