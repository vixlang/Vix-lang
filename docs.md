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

self-lir使用简单的线性扫描
目前会把部分整数load分配到r12到r15
没有使用图着色

## tips

`--backend=self-opt`会先优化MIR
包括copy propagation、DCE、常量分支和简单的alloca提升

目前自带后端只支持x86_64
它还比较简单
遇到问题时可以用LLVM后端对照输出
