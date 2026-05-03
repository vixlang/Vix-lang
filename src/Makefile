CC = clang
CXX = clang++
CFLAGS = -Wall -Wextra -std=c11 -fno-stack-protector -Werror -I../include \
-D_POSIX_C_SOURCE=200809L \
-D_CRT_SECURE_NO_WARNINGS \
-D_CRT_NONSTDC_NO_DEPRECATE

CXXFLAGS = -Wall -Wextra -std=c++17 -fno-stack-protector
CPPFLAGS = -I../include -D_POSIX_C_SOURCE=200809L -DYY_NO_UNISTD_H -D_GNU_SOURCE -Disatty=_isatty -Dfileno=_fileno -D__MSVCRT__ -include io.h

# ====================== 自动 LLVM 配置（核心修改） ======================
LLVM_CXXFLAGS := $(shell llvm-config --cxxflags)
LLVM_LDFLAGS  := $(shell llvm-config --ldflags --libs)

WIN_LIBS = -static -static-libgcc -static-libstdc++ -lpsapi -lntdll -ladvapi32 -lshell32 -lole32 -luser32
BISON = win_bison
FLEX = win_flex

TARGET = vixc
AST_SRC = ast/ast.c ast/typeinfer.c
SEMANTIC_SRC = semantic/semantic.c
PARSER_SRC = parser/parser.tab.c parser/lex.yy.c
LLVM_SRC = compiler/CodeGen.cpp
UTILS_SRC = utils/error.c
C_SRC = main.c $(AST_SRC) $(SEMANTIC_SRC) $(PARSER_SRC) $(UTILS_SRC)
CXX_SRC = $(LLVM_SRC)
C_OBJ = $(C_SRC:.c=.o)
CXX_OBJ = $(CXX_SRC:.cpp=.o)
OBJ = $(C_OBJ) $(CXX_OBJ)

all: $(TARGET)

# 链接：修复所有 Windows 符号缺失
$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LLVM_LDFLAGS) $(WIN_LIBS)

parser/parser.tab.c parser/parser.tab.h: parser/parser.y
	cd parser && $(BISON) -d parser.y

parser/lex.yy.c: parser/lexer.l
	cd parser && $(FLEX) lexer.l

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(LLVM_CXXFLAGS) $(CPPFLAGS) -c $< -o $@

main.o: main.c ../include/ast.h ../include/parser.h ../include/compiler.h ../include/semantic.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

ast/ast.o: ast/ast.c ../include/ast.h parser/parser.tab.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

ast/typeinfer.o: ast/typeinfer.c ../include/typeinfer.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

semantic/semantic.o: semantic/semantic.c ../include/semantic.h ../include/typeinfer.h parser/parser.tab.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

utils/error.o: utils/error.c ../include/compiler.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

compiler/CodeGen.o: compiler/CodeGen.cpp ../include/codegen.h
	$(CXX) $(CXXFLAGS) $(LLVM_CXXFLAGS) $(CPPFLAGS) -c $< -o $@

parser/parser.tab.o: parser/parser.tab.c parser/parser.tab.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

parser/lex.yy.o: parser/lex.yy.c parser/parser.tab.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	del /Q /F *.o ast/*.o semantic/*.o parser/*.o compiler/*.o utils/*.o
	del /Q /F parser/parser.tab.c parser/parser.tab.h parser/lex.yy.c
	del /Q /F $(TARGET).exe

.PHONY: all clean