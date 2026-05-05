# Vix compiler Makefile
# Usage:
#   set LLVM_HOME=F:/Program Files/Msys2/mingw64
#   mingw32-make

# Force using clang/clang++ from LLVM_HOME
ifdef LLVM_HOME
    LLVM_BIN = $(subst \,/,$(LLVM_HOME))/bin
    CC = "$(LLVM_BIN)/clang.exe"
    CXX = "$(LLVM_BIN)/clang++.exe"
else
    CC = clang
    CXX = clang++
endif

CFLAGS = -Wall -Wextra -std=c11 -fno-stack-protector -fexceptions
CXXFLAGS = -Wall -Wextra -std=c++17 -fno-stack-protector -fexceptions
CPPFLAGS = -I../include -D_POSIX_C_SOURCE=200809L

BISON ?= bison
FLEX ?= flex

# LLVM configuration for Windows
ifdef LLVM_HOME
    LLVM_CXXFLAGS = -I"$(subst \,/,$(LLVM_HOME))/include"
    LLVM_LDFLAGS = -L"$(subst \,/,$(LLVM_HOME))/lib"
    LLVM_LIBS = -lLLVM-21
    LLVM_SYSTEM_LIBS = -lpsapi -lshell32 -lole32 -luuid -ladvapi32 -lkernel32 -luser32 -lgdi32 -lwinspool -lcomdlg32 -loleaut32 -lodbc32 -lodbccp32
else
    LLVM_CXXFLAGS = $(shell llvm-config --cxxflags)
    LLVM_LDFLAGS = $(shell llvm-config --ldflags)
    LLVM_LIBS = $(shell llvm-config --libs all)
    LLVM_SYSTEM_LIBS = $(shell llvm-config --system-libs)
endif

TARGET = vixc
AST_SRC = ast/ast.c ast/typeinfer.c
SEMANTIC_SRC = semantic/semantic.c
PARSER_SRC = parser/parser.tab.c parser/lex.yy.c
LLVM_SRC = compiler/CodeGen.cpp compiler/Passes.cpp
LLC_SRC = compiler/Llc/Llc.cpp
TYPECK_SRC = Typeck/Typeck.cpp Typeck/TypeckInfer.cpp Typeck/LayOut.cpp
UTILS_SRC = utils/error.c
C_SRC = main.c $(AST_SRC) $(SEMANTIC_SRC) $(PARSER_SRC) $(UTILS_SRC)
CXX_SRC = $(LLVM_SRC) $(LLC_SRC) $(TYPECK_SRC)
C_OBJ = $(C_SRC:.c=.o)
CXX_OBJ = $(CXX_SRC:.cpp=.o)
OBJ = $(C_OBJ) $(CXX_OBJ)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $(LLVM_LDFLAGS) -o $@ $^ $(LLVM_LIBS) $(LLVM_SYSTEM_LIBS) -lm -lstdc++

parser/parser.tab.c parser/parser.tab.h: parser/parser.y
	cd parser && $(BISON) -d parser.y

parser/lex.yy.c: parser/lexer.l
	cd parser && $(FLEX) lexer.l

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(LLVM_CXXFLAGS) $(CPPFLAGS) -fexceptions -c $< -o $@

main.o: main.c ../include/ast.h ../include/parser.h ../include/compiler.h ../include/semantic.h ../include/typeck.h compiler/Llc/Llc.h
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

compiler/Llc/Llc.o: compiler/Llc/Llc.cpp compiler/Llc/Llc.h
	$(CXX) $(CXXFLAGS) $(LLVM_CXXFLAGS) $(CPPFLAGS) -c $< -o $@

parser/parser.tab.o: parser/parser.tab.c parser/parser.tab.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

parser/lex.yy.o: parser/lex.yy.c parser/parser.tab.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -f $(C_OBJ) $(CXX_OBJ)
	rm -f parser/parser.tab.c parser/parser.tab.h parser/lex.yy.c

.PHONY: all clean
