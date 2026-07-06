# 编译指南

如果你想编译此自举编译器
你得确保你拥有 
- llvm-c
- liblld
- clang
然后运行 seed.sh
``` shell
chmod +x ./seed.sh
#此命令会编译种子编译器
```
然后你会得到vixc，在build/目录下
## tips

假如你改了点vixc的代码
就运行make
会编译自己

