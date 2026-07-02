# Browser Core Wasm Runtime

## 支持边界

- 支持：函数、局部变量、if/elif/else、while、for、break、continue、return、print、数组、struct、字符串字面量、i32、bool
- 不支持：input、import、标准库依赖、文件、网络、extern C、第三方库、宿主 OS 交互

## 值表示

- i32/bool: Wasm i32
- string literal: 指向 data segment 的 i32 指针
- array: 指向堆内存块的 i32 指针
- struct: 指向堆内存块的 i32 指针

## 数组布局

- +0: length(i32)
- +4: element_size(i32)
- +8: element bytes

## struct 布局

- 字段按声明顺序排列
- 初版统一 4 字节对齐
- 所有字段偏移由后端统一计算

## 内存管理

- 线性内存通过 BinaryenSetMemory 声明
- 静态字符串区从固定偏移顺序放置
- 动态堆区使用 bump allocator，只增不减
- 当前浏览器子集不提供释放语义
