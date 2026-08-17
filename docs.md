# MIR后端

MIR后端是vixc自带的x86_64后端
它不会使用LLVM来生成汇编

你得确保你拥有
- nasm
- lld

使用MIR后端
``` shell
build/vixc --backend=self test.vix -o test
```

查看生成的MIR和汇编
``` shell
build/vixc --debug=mir test.vix
build/vixc --backend=self --debug=asm test.vix
```

## 它是怎么工作的

源码会先变成文本MIR
然后解析成MachineFunction、MachineBlock和MachineInst

每个函数会处理两遍
第一遍计算栈槽和栈帧大小
第二遍才输出NASM格式的汇编

生成汇编后
vixc会调用nasm生成目标文件
最后再调用链接器生成程序

## 寄存器和栈

普通MIR后端没有使用图着色
也没有通用寄存器分配器

大部分临时值会放进rbp栈帧
rax、rcx、rdx和r11会作为固定的临时寄存器
浮点运算主要使用xmm0和xmm1

如果想试验寄存器分配
可以使用self-lir
``` shell
build/vixc --backend=self-lir test.vix -o test
build/vixc --debug=lir test.vix
```

self-lir为每个函数从终结指令建立CFG。`br`有一个后继，`br_cond`有两个，
`ret`和`ret void`没有后继；块的文本排列不参与后继判断。后端为每个块计算
use/def，并迭代求解live-in/live-out，所以循环回边和汇合点也会参与活跃性。

寄存器候选只来自`load`结果。支持i1/i8/i16/i32/i64、对应无符号整数、
usize、char、ptr、string和`ptr:*`；f32/f64、结构体值和数组聚合值仍在栈上。
候选区间结合直接使用和块活跃性，并取文本位置上的保守凸包。

self-lir使用r12到r15的四寄存器线性扫描，没有图着色。过期区间会及时释放；
寄存器不足时比较当前区间与结束最晚的活动区间，选择其中结束更晚者spill。
spill不产生新的MIR指令，而是继续使用共享发射器的栈槽。序言只保存实际使用的
callee-saved寄存器，并为奇数个保存寄存器补齐SysV调用栈对齐。

`make self-lir-stage`使用`build/vixc --backend=self-lir`编译`src/main.vix`，
生成独立的`build/vixc-self-lir`。self-lir目前仅支持x86_64 SysV ABI，依赖
NASM生成ELF64目标文件；其他平台、其他ABI和通用浮点寄存器分配尚不支持。

## tips

`--backend=self-opt`会先优化MIR
包括copy propagation、DCE、常量分支和简单的alloca提升

目前自带后端只支持x86_64
它还比较简单
遇到问题时可以用LLVM后端对照输出
