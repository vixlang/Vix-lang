@echo off
REM Windows build script for Vix-lang
REM Usage: build_win.bat [clean]
REM Set LLVM_HOME to your LLVM installation path if not in default location

if not defined LLVM_HOME (
    set LLVM_HOME=C:\Program Files\LLVM
)

if "%1"=="clean" (
    echo Cleaning...
    del /Q ast\*.o semantic\*.o parser\*.o utils\*.o compiler\*.o compiler\Llc\*.o Typeck\*.o main.o vixc.exe 2>nul
    del /Q parser\parser.tab.c parser\parser.tab.h parser\lex.yy.c 2>nul
    goto :eof
)

echo Building Vix-lang for Windows...
echo Using LLVM from: %LLVM_HOME%

REM Check if LLVM exists
if not exist "%LLVM_HOME%\include\llvm\IR\IRBuilder.h" (
    echo ERROR: LLVM headers not found at %LLVM_HOME%
    echo Please install LLVM or set LLVM_HOME environment variable
    echo Download LLVM from: https://github.com/llvm/llvm-project/releases
    exit /b 1
)

REM Generate parser files using MSYS2 bison/flex
where bison >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: bison not found. Install via MSYS2: pacman -S bison flex
    exit /b 1
)

cd parser
bison -d parser.y
flex lexer.l
cd ..

REM Compile C files
clang -Wall -Wextra -std=c11 -fno-stack-protector -I../include -D_POSIX_C_SOURCE=200809L -D_WIN32 -c main.c -o main.o
clang -Wall -Wextra -std=c11 -fno-stack-protector -I../include -D_POSIX_C_SOURCE=200809L -D_WIN32 -c ast/ast.c -o ast/ast.o
clang -Wall -Wextra -std=c11 -fno-stack-protector -I../include -D_POSIX_C_SOURCE=200809L -D_WIN32 -c ast/typeinfer.c -o ast/typeinfer.o
clang -Wall -Wextra -std=c11 -fno-stack-protector -I../include -D_POSIX_C_SOURCE=200809L -D_WIN32 -c semantic/semantic.c -o semantic/semantic.o
clang -Wall -Wextra -std=c11 -fno-stack-protector -I../include -D_POSIX_C_SOURCE=200809L -D_WIN32 -c parser/parser.tab.c -o parser/parser.tab.o
clang -Wall -Wextra -std=c11 -fno-stack-protector -I../include -D_POSIX_C_SOURCE=200809L -D_WIN32 -c parser/lex.yy.c -o parser/lex.yy.o
clang -Wall -Wextra -std=c11 -fno-stack-protector -I../include -D_POSIX_C_SOURCE=200809L -D_WIN32 -c utils/error.c -o utils/error.o

REM Compile C++ files with LLVM
clang++ -Wall -Wextra -std=c++17 -fno-stack-protector -fexceptions -I../include -I"%LLVM_HOME%/include" -D_POSIX_C_SOURCE=200809L -D_WIN32 -c compiler/CodeGen.cpp -o compiler/CodeGen.o
clang++ -Wall -Wextra -std=c++17 -fno-stack-protector -fexceptions -I../include -I"%LLVM_HOME%/include" -D_POSIX_C_SOURCE=200809L -D_WIN32 -c compiler/Passes.cpp -o compiler/Passes.o
clang++ -Wall -Wextra -std=c++17 -fno-stack-protector -fexceptions -I../include -I"%LLVM_HOME%/include" -D_POSIX_C_SOURCE=200809L -D_WIN32 -c compiler/Llc/Llc.cpp -o compiler/Llc/Llc.o
clang++ -Wall -Wextra -std=c++17 -fno-stack-protector -fexceptions -I../include -I"%LLVM_HOME%/include" -D_POSIX_C_SOURCE=200809L -D_WIN32 -c Typeck/Typeck.cpp -o Typeck/Typeck.o
clang++ -Wall -Wextra -std=c++17 -fno-stack-protector -fexceptions -I../include -I"%LLVM_HOME%/include" -D_POSIX_C_SOURCE=200809L -D_WIN32 -c Typeck/TypeckInfer.cpp -o Typeck/TypeckInfer.o
clang++ -Wall -Wextra -std=c++17 -fno-stack-protector -fexceptions -I../include -I"%LLVM_HOME%/include" -D_POSIX_C_SOURCE=200809L -D_WIN32 -c Typeck/LayOut.cpp -o Typeck/LayOut.o

REM Link everything
echo Linking...
clang++ -o vixc.exe main.o ast/ast.o ast/typeinfer.o semantic/semantic.o parser/parser.tab.o parser/lex.yy.o utils/error.o compiler/CodeGen.o compiler/Passes.o compiler/Llc/Llc.o Typeck/Typeck.o Typeck/TypeckInfer.o Typeck/LayOut.o -L"%LLVM_HOME%/lib" -lLLVM-C -lLLVMSupport -lLLVMCore -lLLVMCodeGen -lLLVMIRReader -lLLVMAsmParser -lLLVMAsmPrinter -lLLVMBitReader -lLLVMBitWriter -lLLVMExecutionEngine -lLLVMMC -lLLVMMCParser -lLLVMTarget -lLLVMipo -lLLVMVectorize -lLLVMInstCombine -lLLVMScalarOpts -lLLVMTransformUtils -lLLVMAnalysis -lLLVMSymbolize -lLLVMDebugInfoDWARF -lLLVMObject -lLLVMBinaryFormat -lLLVMOption -lLLVMDemangle -lLLVMPasses -lLLVMProfileData -lLLVMRemarks -lLLVMCoroutines -lLLVMOrcJIT -lLLVMJITLink -lLLVMOrcShared -lLLVMOrcTargetProcess -lLLVMRuntimeDyld -lLLVMX86CodeGen -lLLVMX86Desc -lLLVMX86Info -lLLVMX86AsmParser -lLLVMX86Disassembler -lLLVMX86Utils -lLLVMGlobalISel -lLLVMSelectionDAG -lpsapi -lshell32 -lole32 -luuid -ladvapi32 -lkernel32 -luser32 -lgdi32 -lwinspool -lcomdlg32 -loleaut32 -lodbc32 -lodbccp32 -lm

if %errorlevel% equ 0 (
    echo Build successful! vixc.exe created.
) else (
    echo Build failed!
    exit /b 1
)
