---
name: vix-definition-of-done
description: Pre-commit checklist for changes to Vix programs, the compiler or the analyzer.
---

# 收工前检查表

## 写 Vix 程序

- [ ] `build/vixc-patched <file> --check` 返回 0
- [ ] 需要的话能跑起来（`sh scripts/run-vix.sh <file>`）
- [ ] 方法用 receiver 声明，没有 `impl` 块
- [ ] 布尔运算用 `and` / `or`
- [ ] 闭包写了显式捕获列表
- [ ] 调 `&mut` 方法的值是 `let mut`

## 改编译器

- [ ] `build/vixc src/main.vix -obj -o /tmp/vixc.o` 返回 0（自举）
- [ ] `sh scripts/build-analyzer.sh` 返回 0（analyzer 聚合）
- [ ] 新文件已加入 `src/sys.vix`
- [ ] 新字段在所有构造器/复制函数里都带上了
- [ ] `desugar` 重建 AST 时保留了新字段
- [ ] 类型串变化已同步到 `analysis_lower_program_for_backend`
- [ ] 编译差分无 `old=0 new=1`
- [ ] 涉及方法时 `sh scripts/check-method-invariants.sh` 通过
- [ ] 涉及方法时 `python3 tests/methods_e2e.py` 通过

## 改 analyzer / 扩展

- [ ] `build/vixc-patched vix-analyzer/main.vix -obj -o /tmp/an.o` 返回 0
- [ ] `python3 vix-analyzer/tests/lsp_format_safety.py build/vix-analyzer` 通过
- [ ] 方法相关改动用 `python3 vix-analyzer/tests/lsp_methods.py build/vix-analyzer` 验证
- [ ] 语法高亮改动跑 `python3 editors/vscode/tests/grammar_test.py`
- [ ] 打包用 `sh scripts/package-extension.sh`（**绝不用 `--no-dependencies`**）
- [ ] 版本号已 bump（VS Code 不会覆盖相同版本号）

## 改动过 skill/ 文档

- [ ] `python3 scripts/check-skill-docs.py` 通过
- [ ] 新增的示例是自包含可编译的，不是片段
- [ ] 索引（`skill/SKILL.md`）已更新

## 提交

- [ ] `git diff --check` 干净
- [ ] 只暂存了相关文件
- [ ] 没有把 `build/`、`runtime/*.o`、`*.vsix`、`vstd`、`.idea/`、`.vscode/` 加进去
- [ ] 提交信息说明了**为什么**改，以及修掉了什么
- [ ] 已推送

## 绝对不要

- [ ] 没有「只要这个例子能跑」的特判
- [ ] 没有按名字判断语言行为（`"push"` 这类）
- [ ] 没有用裸数字表示枚举语义
- [ ] 没有在多处重复同一份逻辑
- [ ] 没有把未验证的东西说成已验证
