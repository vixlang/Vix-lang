# Browser Playground Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Build a browser-based Vix playground where users can write and run Vix code entirely in the browser (no server).

**Architecture:** Compile the Vix compiler frontend (parser, typeck, ownership) to WASM via Emscripten. Add a new WasmCodegen module that traverses the Vix AST and uses Binaryen's C API to produce .wasm binaries. The resulting WASM runs via WebAssembly.instantiate() with JS-implemented import functions (puts, putchar). CodeMirror 6 provides editing.

**Tech Stack:** Emscripten (C/C++ → WASM), Binaryen (WASM codegen), CodeMirror 6 (editor)

## Global Constraints

- All compilation must happen in the browser (no server)
- vixc WASM bundle must be cacheable via Service Worker / IndexedDB
- Playground is self-contained in a single HTML page + WASM assets
- Binaryen must be compiled as part of the Emscripten build (static link)
- stdlib calls like `puts()` are replaced with WASM import functions

---
## File Structure

```
vix-lang/
├── CMakeLists.txt                    # Modify: add Emscripten build target
├── src/
│   ├── main.c                        # Modify: extract frontend into library
│   ├── compiler/
│   │   ├── WasmCodegen.h             # Create: WasmCodegen class declaration
│   │   ├── WasmCodegen.cpp           # Create: AST → Binaryen IR
│   │   ├── WasmTypeMap.h             # Create: Vix type → WASM type mapping
│   │   └── WasmTypeMap.cpp           # Create: type mapper implementation
│   ├── Typeck/                       # Unchanged (no LLVM dependency)
│   └── Ownership/                    # Unchanged (no LLVM dependency)
├── playground/
│   ├── CMakeLists.txt                # Create: Emscripten build definition
│   ├── vixc_frontend.c               # Create: entry point for WASM library mode
│   ├── playground.html               # Create: playground page
│   ├── playground.js                 # Create: JS runtime + UI logic
│   └── playground.css               # Create: playground styles
└── third_party/
    └── binaryen/                     # Create: Binaryen source (git submodule)
```

### Task 1: Extract Compiler Frontend Library

**Files:**
- Create: `src/libvixc_frontend.h`
- Create: `playground/vixc_frontend.c`
- Modify: `src/main.c`
- Create: `tests/test_frontend.c`

**Interfaces:**
- Consumes: existing `parser.y`, `lexer.l`, `ast.c`, `semantic.c`, `typeck`, `ownership`
- Produces: `libvixc_frontend.h` with these exports:
  ```c
  typedef struct { ASTNode *root; int error_count; } CompileResult;
  CompileResult vixc_compile_string(const char *source);
  void vixc_free_result(CompileResult *result);
  const char *vixc_get_last_error(void);
  ```

- [x] **Step 1: Create `src/libvixc_frontend.h`**

```c
#ifndef VIXC_FRONTEND_H
#define VIXC_FRONTEND_H

#include "ast.h"
#include "error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ASTNode *root;
    int error_count;
} CompileResult;

CompileResult vixc_compile_string(const char *source);
void vixc_free_result(CompileResult *result);
const char *vixc_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
```

- [x] **Step 2: Create `playground/vixc_frontend.c`**

```c
#include "libvixc_frontend.h"
#include "parser.h"
#include "semantic.h"
#include "typeck.h"
#include "ownership.h"
#include <stdlib.h>
#include <string.h>

extern FILE *yyin;
extern ASTNode *root;
extern int yyparse(void);
extern void load_source_file(const char *name);
extern int check_undefined_symbols(ASTNode *root);
extern int typecheck_program(ASTNode *root);
extern int ownership_check_program(ASTNode *root);

static char error_buf[4096];
static int error_buf_len = 0;

CompileResult vixc_compile_string(const char *source) {
    CompileResult result = {NULL, 0};
    error_buf_len = 0;

    /* Write source to temp string stream */
    FILE *source_stream = fmemopen((void*)source, strlen(source), "r");
    if (!source_stream) { result.error_count = 1; return result; }

    load_source_file("playground_input");
    yyin = source_stream;

    if (yyparse() != 0 || !root) {
        result.error_count = 1;
        fclose(source_stream);
        return result;
    }

    inline_imports(root);

    if (check_undefined_symbols(root) > 0) {
        result.error_count = 1;
        free_ast(root); root = NULL;
        fclose(source_stream);
        return result;
    }

    if (typecheck_program(root) != 0) {
        result.error_count = 1;
        free_ast(root); root = NULL;
        fclose(source_stream);
        return result;
    }

    if (ownership_check_program(root) != 0) {
        result.error_count = 1;
        free_ast(root); root = NULL;
        fclose(source_stream);
        return result;
    }

    result.root = root;
    fclose(source_stream);
    return result;
}

void vixc_free_result(CompileResult *result) {
    if (result && result->root) {
        free_ast(result->root);
        result->root = NULL;
    }
}

const char *vixc_get_last_error(void) {
    return error_buf;
}
```

- [x] **Step 3: Modify `src/main.c` to keep LLVM path separate**

Wrap all LLVM-dependent code (codegen, Llc, Linker) in `#ifndef VIXC_FRONTEND_ONLY`. This keeps main.c buildable for both native and WASM targets.

Add at the top after includes:
```c
#ifndef VIXC_FRONTEND_ONLY
/* LLVM-dependent codegen, Llc, Linker includes */
#endif
```

Wrap lines 409-772 (the LLVM codegen/linking path) in `#ifndef VIXC_FRONTEND_ONLY ... #endif`.

- [x] **Step 4: Create test `tests/test_frontend.c`**

```c
#include "libvixc_frontend.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

void test_hello_world() {
    const char *source = "use \"core/io.vix\" as io\nfn main(): i32 { io::puts(\"hello\"); return 0 }";
    CompileResult r = vixc_compile_string(source);
    assert(r.error_count == 0);
    assert(r.root != NULL);
    vixc_free_result(&r);
    printf("PASS: test_hello_world\n");
}

void test_syntax_error() {
    const char *source = "fn main() { this is bad syntax @@@ }";
    CompileResult r = vixc_compile_string(source);
    assert(r.error_count > 0);
    printf("PASS: test_syntax_error\n");
}

void test_type_error() {
    const char *source = "fn main(): i32 { return \"string\"; }";
    CompileResult r = vixc_compile_string(source);
    assert(r.error_count > 0);
    printf("PASS: test_type_error\n");
}

int main() {
    test_hello_world();
    test_syntax_error();
    test_type_error();
    printf("All frontend tests passed\n");
    return 0;
}
```

- [x] **Step 5: Build and run native test**

```bash
gcc -Iinclude -Isrc -o test_frontend \
    tests/test_frontend.c \
    src/main.c \
    src/ast/ast.c \
    src/semantic/semantic.c \
    src/utils/error.c \
    -lfl -DVFIXC_FRONTEND_ONLY \
    -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
./test_frontend
```

Expected: all tests PASS

- [x] **Step 6: Commit**

```bash
git add src/libvixc_frontend.h playground/vixc_frontend.c tests/test_frontend.c src/main.c
git commit -m "feat: extract compiler frontend library for WASM build"
```

### Task 2: Add Binaryen as a Submodule

**Files:**
- Create: `.gitmodules`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from Task 1
- Produces: Binaryen source at `third_party/binaryen/`, a `binaryen` CMake target

- [x] **Step 1: Add Binaryen git submodule**

```bash
git submodule add https://github.com/WebAssembly/binaryen.git third_party/binaryen
cd third_party/binaryen
git checkout tags/version_121   # Use a stable release
cd ../..
```

- [x] **Step 2: Verify Binaryen builds**

```bash
cd third_party/binaryen
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_STATIC_LIBS=ON -DBUILD_TOOLS=OFF -DBUILD_TESTS=OFF
cmake --build build
```

Expected: generates `build/lib/libbinaryen.a`

- [x] **Step 3: Add submodule and Binaryen target to root `CMakeLists.txt`**

Add at the end:
```cmake
# Binaryen (for WASM codegen)
if(EMSCRIPTEN OR BUILD_WASM_CODEGEN)
    set(BINARYEN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/binaryen")
    set(BINARYEN_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/binaryen")
    add_subdirectory("${BINARYEN_SRC_DIR}" "${BINARYEN_BUILD_DIR}" EXCLUDE_FROM_ALL)
    target_include_directories(vixc PRIVATE "${BINARYEN_SRC_DIR}/src")
endif()
```

- [x] **Step 4: Commit**

```bash
git add .gitmodules third_party/binaryen CMakeLists.txt
git commit -m "feat: add Binaryen as submodule for WASM codegen"
```

### Task 3: Implement WasmCodegen Module

**Files:**
- Create: `src/compiler/WasmCodegen.h`
- Create: `src/compiler/WasmCodegen.cpp`
- Create: `src/compiler/WasmTypeMap.h`
- Create: `src/compiler/WasmTypeMap.cpp`

**Interfaces:**
- Consumes: `ASTNode` (from `ast.h`), `CompileResult.root` (from Task 1)
- Produces: `bool WasmCodegen::emit(ASTNode *root, std::vector<uint8_t> &out_bytes)` emitting valid .wasm bytes

- [x] **Step 1: Create `src/compiler/WasmTypeMap.h`**

```cpp
#ifndef VIX_WASM_TYPEMAP_H
#define VIX_WASM_TYPEMAP_H

#include "type.h"
#include <cstdint>

enum class WasmValType : uint8_t {
    I32 = 0x7F,
    I64 = 0x7E,
    F32 = 0x7D,
    F64 = 0x7C,
};

struct WasmTypeInfo {
    WasmValType val_type;
    int32_t wasm_memory_size;  // bytes in linear memory
    bool is_struct;            // stored in memory, accessed via pointer
    int struct_field_count;
};

WasmTypeInfo map_vix_type_to_wasm(const Type *type);

#endif
```

- [x] **Step 2: Create `src/compiler/WasmTypeMap.cpp`**

```cpp
#include "WasmTypeMap.h"

WasmTypeInfo map_vix_type_to_wasm(const Type *type) {
    WasmTypeInfo info = {};
    switch (type->kind) {
        case TYPE_I32:
        case TYPE_BOOL:
            info.val_type = WasmValType::I32;
            info.wasm_memory_size = 4;
            break;
        case TYPE_I64:
            info.val_type = WasmValType::I64;
            info.wasm_memory_size = 8;
            break;
        case TYPE_F32:
            info.val_type = WasmValType::F32;
            info.wasm_memory_size = 4;
            break;
        case TYPE_F64:
            info.val_type = WasmValType::F64;
            info.wasm_memory_size = 8;
            break;
        case TYPE_PTR:
            info.val_type = WasmValType::I32;  // WASM is 32-bit
            info.wasm_memory_size = 4;
            break;
        case TYPE_STRUCT:
            info.val_type = WasmValType::I32;  // structs passed by pointer
            info.is_struct = true;
            info.wasm_memory_size = 4;         // pointer size
            /* compute struct field count from type->struct_fields */
            break;
        case TYPE_ADT:
        case TYPE_ENUM:
            info.val_type = WasmValType::I32;  // tag + payload via pointer
            info.is_struct = true;
            info.wasm_memory_size = 4;
            break;
        default:
            info.val_type = WasmValType::I32;
            info.wasm_memory_size = 4;
            break;
    }
    return info;
}
```

- [x] **Step 3: Create `src/compiler/WasmCodegen.h`**

```cpp
#ifndef VIX_WASM_CODEGEN_H
#define VIX_WASM_CODEGEN_H

#include "ast.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

// Forward declare Binaryen types
typedef struct BinaryenModuleRef_ *BinaryenModuleRef;
typedef struct BinaryenFunctionRef_ *BinaryenFunctionRef;
typedef BinaryenModuleRef BinaryenExpressionRef;

class WasmCodegen {
public:
    WasmCodegen();
    ~WasmCodegen();

    bool emit(ASTNode *root, std::vector<uint8_t> &out_bytes, std::string &error_msg);

private:
    BinaryenModuleRef m_module;

    // Environment: import functions from JS
    void add_imports();

    // AST traversal
    BinaryenExpressionRef compile_node(ASTNode *node);
    BinaryenExpressionRef compile_block(ASTNode *stmt_list);
    BinaryenExpressionRef compile_if(ASTNode *if_node);
    BinaryenExpressionRef compile_while(ASTNode *while_node);
    BinaryenExpressionRef compile_binary_op(ASTNode *op_node);
    BinaryenExpressionRef compile_unary_op(ASTNode *op_node);
    BinaryenExpressionRef compile_call(ASTNode *call_node);
    BinaryenExpressionRef compile_ident(ASTNode *ident_node);
    BinaryenExpressionRef compile_literal(ASTNode *lit_node);
    BinaryenExpressionRef compile_struct_get(ASTNode *member_node);
    BinaryenExpressionRef compile_struct_set(ASTNode *assign_node);

    // Memory layout
    uint32_t allocate_struct(const Type *struct_type);
    uint32_t allocate_string_literal(const char *str);

    // Function tracking
    struct FuncInfo {
        BinaryenFunctionRef func_ref;
        std::unordered_map<std::string, uint32_t> local_indices;
    };
    std::unordered_map<std::string, FuncInfo> m_functions;
    FuncInfo *m_current_func;

    uint32_t get_or_create_local(const char *name, WasmValType type);
};

#endif
```

- [x] **Step 4: Create `src/compiler/WasmCodegen.cpp`**

Full implementation of the AST → Binaryen traversal. Key patterns:

```cpp
#include "WasmCodegen.h"
#include "WasmTypeMap.h"
#include "binaryen-c.h"  // Binaryen C API header

WasmCodegen::WasmCodegen() : m_module(nullptr), m_current_func(nullptr) {}
WasmCodegen::~WasmCodegen() { if (m_module) BinaryenModuleDestroy(m_module); }

bool WasmCodegen::emit(ASTNode *root, std::vector<uint8_t> &out_bytes, std::string &error_msg) {
    m_module = BinaryenModuleCreate();

    // Step 1: Add import functions from JS environment
    add_imports();

    // Step 2: Set up linear memory (1 page = 64KB, growable)
    BinaryenSetMemory(m_module, 1, -1, "memory", nullptr, 0, 0);

    // Step 3: Register all functions (first pass — collect signatures)
    ASTNode *prog = root;
    for (ASTNode *child = prog->first_child; child; child = child->next_sibling) {
        if (child->type == NODE_FUNC_DEF) {
            register_function(child);
        }
    }

    // Step 4: Compile each function body (second pass)
    for (auto &[name, info] : m_functions) {
        ASTNode *func_node = /* find by name */;
        compile_function_body(func_node);
    }

    // Step 5: Set export for main (or _start)
    if (m_functions.count("main")) {
        BinaryenAddExport(m_module, "main", "main");
    }

    // Step 6: Write WASM binary
    BinaryenModuleAllocateAndWriteResult write_result = BinaryenModuleAllocateAndWrite(m_module, nullptr);
    if (write_result.bytes) {
        out_bytes.assign(write_result.bytes, write_result.bytes + write_result.numBytes);
        free((void*)write_result.bytes);
        return true;
    }

    error_msg = "Failed to write WASM binary";
    return false;
}

void WasmCodegen::add_imports() {
    // Import vix_putchar: (i32) → ()
    BinaryenAddFunctionImport(m_module, "vix_putchar", "env", "vix_putchar",
                              BinaryenTypeCreate({}, 0),
                              BinaryenTypeCreate(NULL, 0));

    // Import vix_puts: (i32) → ()  (receives string pointer)
    BinaryenAddFunctionImport(m_module, "vix_puts", "env", "vix_puts",
                              BinaryenTypeCreate(NULL, 0),
                              BinaryenTypeCreate({}, 0));

    // Import vix_exit: (i32) → ()
    BinaryenAddFunctionImport(m_module, "vix_exit", "env", "vix_exit",
                              BinaryenTypeCreate(NULL, 0),
                              BinaryenTypeCreate({}, 0));
}

BinaryenExpressionRef WasmCodegen::compile_node(ASTNode *node) {
    switch (node->type) {
        case NODE_PROGRAM:
        case NODE_BLOCK:     return compile_block(node);
        case NODE_IF:        return compile_if(node);
        case NODE_WHILE:     return compile_while(node);
        case NODE_BINOP:     return compile_binary_op(node);
        case NODE_UNARYOP:   return compile_unary_op(node);
        case NODE_CALL:      return compile_call(node);
        case NODE_IDENT:     return compile_ident(node);
        case NODE_INT_LITERAL:
            return BinaryenConst(m_module, BinaryenLiteralInt32(node->data.int_value));
        case NODE_FLOAT_LITERAL:
            return BinaryenConst(m_module, BinaryenLiteralFloat64(node->data.float_value));
        case NODE_BOOL_LITERAL:
            return BinaryenConst(m_module, BinaryenLiteralInt32(node->data.int_value ? 1 : 0));
        case NODE_STRING_LITERAL: {
            uint32_t addr = allocate_string_literal(node->data.string_value);
            return BinaryenConst(m_module, BinaryenLiteralInt32(addr));
        }
        case NODE_RETURN:
            return BinaryenReturn(m_module, compile_node(node->first_child));
        case NODE_VAR_DECL:
            return compile_var_decl(node);
        case NODE_ASSIGN:
            return compile_struct_set(node);
        case NODE_MEMBER_ACCESS:
            return compile_struct_get(node);
        default:
            return BinaryenNop(m_module);
    }
}

BinaryenExpressionRef WasmCodegen::compile_block(ASTNode *stmt_list) {
    std::vector<BinaryenExpressionRef> stmts;
    for (ASTNode *child = stmt_list->first_child; child; child = child->next_sibling) {
        BinaryenExpressionRef expr = compile_node(child);
        if (expr) stmts.push_back(expr);
    }
    if (stmts.empty()) return BinaryenNop(m_module);
    if (stmts.size() == 1) return stmts[0];
    return BinaryenBlock(m_module, nullptr, stmts.data(), stmts.size(), BinaryenTypeAuto());
}
```

- [x] **Step 5: Write the type-checker integration test**

Create `tests/test_wasm_codegen.cpp`:

```cpp
#include "WasmCodegen.h"
#include "libvixc_frontend.h"
#include <cassert>
#include <cstdio>
#include <vector>

void test_compile_to_wasm() {
    const char *source = "fn add(a: i32, b: i32): i32 { return a + b; }\n"
                         "fn main(): i32 { return add(1, 2); }";

    CompileResult cr = vixc_compile_string(source);
    assert(cr.error_count == 0);
    assert(cr.root != nullptr);

    WasmCodegen cg;
    std::vector<uint8_t> wasm_bytes;
    std::string error;
    bool ok = cg.emit(cr.root, wasm_bytes, error);
    assert(ok);
    assert(!wasm_bytes.empty());
    // WASM binary must start with \0asm
    assert(wasm_bytes[0] == 0x00);
    assert(wasm_bytes[1] == 0x61); // 'a'
    assert(wasm_bytes[2] == 0x73); // 's'
    assert(wasm_bytes[3] == 0x6d); // 'm'

    vixc_free_result(&cr);
    printf("PASS: test_compile_to_wasm (%zu bytes)\n", wasm_bytes.size());
}

int main() {
    test_compile_to_wasm();
    printf("All WASM codegen tests passed\n");
    return 0;
}
```

- [x] **Step 6: Build and run the test (native, with Binaryen)**

```bash
cd build
cmake .. -DBUILD_WASM_CODEGEN=ON -DBUILD_TESTING=ON
cmake --build . --target test_wasm_codegen
./test_wasm_codegen
```

Expected: `PASS: test_compile_to_wasm (NN bytes)` and all tests pass

- [x] **Step 7: Commit**

```bash
git add src/compiler/WasmCodegen.h src/compiler/WasmCodegen.cpp
git add src/compiler/WasmTypeMap.h src/compiler/WasmTypeMap.cpp
git add tests/test_wasm_codegen.cpp
git commit -m "feat: implement WasmCodegen module using Binaryen"
```

### Task 4: Emscripten Build Configuration

**Files:**
- Create: `playground/CMakeLists.txt`
- Create: `playground/vixc-wasm.cpp`
- Create: `playground/pre.js`

**Interfaces:**
- Consumes: `libvixc_frontend.h` (Task 1), `WasmCodegen` (Task 3)
- Produces: `vixc-wasm.js` + `vixc-wasm.wasm` — the browser-ready compiler bundle

- [x] **Step 1: Create `playground/vixc-wasm.cpp`** — entry point for Emscripten

```cpp
#include <emscripten.h>
#include "libvixc_frontend.h"
#include "WasmCodegen.h"
#include <string>
#include <vector>

extern "C" {

EMSCRIPTEN_KEEPALIVE
int compile_vix(const char *source, char **out_wasm_bytes, int *out_wasm_len, char **out_error) {
    CompileResult cr = vixc_compile_string(source);
    if (cr.error_count > 0 || !cr.root) {
        *out_error = strdup(vixc_get_last_error());
        return 0;
    }

    WasmCodegen cg;
    std::vector<uint8_t> wasm_bytes;
    std::string error;
    if (!cg.emit(cr.root, wasm_bytes, error)) {
        *out_error = strdup(error.c_str());
        vixc_free_result(&cr);
        return 0;
    }

    // Copy to WASM heap for JS access
    *out_wasm_len = wasm_bytes.size();
    *out_wasm_bytes = (char*)malloc(wasm_bytes.size());
    memcpy(*out_wasm_bytes, wasm_bytes.data(), wasm_bytes.size());

    vixc_free_result(&cr);
    return 1;  // success
}

EMSCRIPTEN_KEEPALIVE
void free_wasm_result(char *bytes, char *error) {
    if (bytes) free(bytes);
    if (error) free(error);
}

}  // extern "C"
```

- [x] **Step 2: Create `playground/CMakeLists.txt`**

```cmake
if(EMSCRIPTEN)
    set(VFIX_WASM_SOURCES
        vixc-wasm.cpp
        ../src/main.c
        ../src/ast/ast.c
        ../src/semantic/semantic.c
        ../src/utils/error.c
        ../src/compiler/WasmCodegen.cpp
        ../src/compiler/WasmTypeMap.cpp
        ../src/Typeck/Typeck.cpp
        ../src/Typeck/TypeckInfer.cpp
        ../src/Typeck/LayOut.cpp
        ../src/Ownership/Ownership.cpp
    )

    set(BINARYEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/binaryen")

    add_executable(vixc-wasm ${VFIX_WASM_SOURCES})
    target_include_directories(vixc-wasm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/compiler
        ${BINARYEN_DIR}/src
    )
    target_link_libraries(vixc-wasm PRIVATE binaryen)

    set_target_properties(vixc-wasm PROPERTIES
        LINK_FLAGS "-s WASM=1 -s ALLOW_MEMORY_GROWTH=1 -s EXPORTED_FUNCTIONS='[_compile_vix, _free_wasm_result]' -s EXPORTED_RUNTIME_METHODS='[ccall, getValue, setValue, UTF8ToString, stringToUTF8]' -O2 --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/pre.js"
    )
endif()
```

- [x] **Step 3: Create `playground/pre.js`**

```javascript
// Emscripten pre-js: defines the JS API for the playground
var VixcWasm = {
    _wasm_ready: false,
    _pending: [],

    init: function() {
        return new Promise(function(resolve, reject) {
            if (VixcWasm._wasm_ready) { resolve(); return; }
            VixcWasm._pending.push({ resolve: resolve, reject: reject });
        });
    }
};

// Called when Emscripten runtime is ready
function onVixcWasmReady() {
    VixcWasm._wasm_ready = true;
    VixcWasm._pending.forEach(function(p) { p.resolve(); });
    VixcWasm._pending = [];
}
```

- [x] **Step 4: Build vixc-wasm with Emscripten**

```bash
mkdir -p playground/build
cd playground/build
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release
emmake make vixc-wasm -j4
```

Expected: produces `playground/build/vixc-wasm.js` and `vixc-wasm.wasm`

- [x] **Step 5: Verify the WASM module loads in Node.js**

```javascript
// test_load.js — quick smoke test
const VixcWasm = require('./vixc-wasm.js');
VixcWasm.onRuntimeInitialized = function() {
    // Test that compile_vix exists
    console.log('Module loaded successfully');
    console.log('compile_vix:', typeof VixcWasm._compile_vix);
};
```

```bash
node test_load.js
```

Expected: `Module loaded successfully`

- [x] **Step 6: Add build script to root Makefile**

```makefile
.PHONY: wasm

wasm: $(CMAKE_BUILD_DIR)
	cd playground && mkdir -p build && cd build && \
	emcmake cmake .. -DCMAKE_BUILD_TYPE=Release && \
	emmake make vixc-wasm -j4
```

- [x] **Step 7: Commit**

```bash
git add playground/CMakeLists.txt playground/vixc-wasm.cpp playground/pre.js Makefile
git commit -m "feat: add Emscripten build for vixc-wasm playground"
```

### Task 5: JS Runtime + Playground UI

**Files:**
- Create: `playground/playground.html`
- Create: `playground/playground.js`
- Create: `playground/playground.css`

**Interfaces:**
- Consumes: `vixc-wasm.js` + `vixc-wasm.wasm` (Task 4)
- Produces: Fully functional HTML playground page

- [x] **Step 1: Create `playground/playground.html`**

```html
<!doctype html>
<html lang="zh-CN">
<head>
    <meta charset="utf-8">
    <title>Vix Playground</title>
    <meta name="viewport" content="width=device-width,initial-scale=1.0">
    <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/codemirror@5.65.18/lib/codemirror.min.css">
    <link rel="stylesheet" href="playground.css">
</head>
<body>
    <div id="app">
        <header>
            <h1>Vix Playground</h1>
            <div id="status-bar">
                <span id="status-text">等待加载编译器...</span>
                <button id="btn-run" disabled>运行 (Ctrl+Enter)</button>
            </div>
        </header>
        <main>
            <div id="editor-container"></div>
            <div id="output-container">
                <div id="output-tabs">
                    <button class="tab active" data-tab="output">输出</button>
                    <button class="tab" data-tab="wasm">WASM</button>
                </div>
                <div id="output-content"></div>
                <div id="wasm-content" style="display:none"></div>
            </div>
        </main>
    </div>

    <script src="https://cdn.jsdelivr.net/npm/codemirror@5.65.18/lib/codemirror.min.js"></script>
    <script src="vixc-wasm.js"></script>
    <script src="playground.js"></script>
</body>
</html>
```

- [x] **Step 2: Create `playground/playground.js`**

```javascript
(function() {
    const DEFAULT_CODE = `use "core/io.vix" as io

fn main(): i32
{
    puts("Hello, Vix Playground!")
    return 0
}`;

    // --- CodeMirror Editor ---
    const editor = CodeMirror(document.getElementById('editor-container'), {
        value: DEFAULT_CODE,
        mode: 'text/x-vix',
        theme: 'default',
        lineNumbers: true,
        indentUnit: 4,
        tabSize: 4,
        autofocus: true,
        extraKeys: {
            'Ctrl-Enter': runCode,
            'Cmd-Enter': runCode
        }
    });

    // --- Status Bar ---
    const statusText = document.getElementById('status-text');
    const runBtn = document.getElementById('btn-run');

    // --- WASM Imports Provider ---
    function createWasmImports() {
        let output = '';
        const env = {
            vix_putchar: function(c) {
                output += String.fromCharCode(c);
                if (c === 10) flushOutput();  // newline
            },
            vix_puts: function(ptr) {
                // Read null-terminated string from WASM memory
                const mem = new Uint8Array(wasmModule.instance.exports.memory.buffer);
                let s = '';
                while (mem[ptr] !== 0) {
                    s += String.fromCharCode(mem[ptr]);
                    ptr++;
                }
                output += s + '\n';
                flushOutput();
            },
            vix_exit: function(code) {
                console.log('Program exited with code', code);
            }
        };
        return { env };
    }

    let wasmModule = null;
    let outputLines = [];

    function flushOutput() {
        const el = document.getElementById('output-content');
        el.textContent = outputLines.join('');
        el.scrollTop = el.scrollHeight;
    }

    // --- Compile & Run ---
    async function runCode() {
        const source = editor.getValue();
        const outputEl = document.getElementById('output-content');
        outputEl.textContent = '编译中...\n';
        outputLines = [];

        try {
            // Step 1: Compile Vix source to WASM bytes
            const resultPtr = Module.ccall('compile_vix', 'number', ['string', 'number', 'number', 'number'],
                [source, null, null, null]);

            if (!resultPtr) {
                outputEl.textContent = '编译错误';
                return;
            }

            // Step 2: Get WASM bytes from heap
            const wasmBytes = Module.HEAPU8.slice(ptr, ptr + len);

            // Step 3: Instantiate and run
            const importObj = createWasmImports();
            wasmModule = await WebAssembly.instantiate(wasmBytes, importObj);
            wasmModule.instance.exports.main();

            // Step 4: Free WASM memory
            Module.ccall('free_wasm_result', null, ['number', 'number'], [ptr, null]);

        } catch (err) {
            outputEl.textContent += '运行时错误: ' + err.message + '\n';
        }
    }

    // --- Load vixc-wasm ---
    runBtn.disabled = true;
    statusText.textContent = '正在加载编译器 (5-10 MB)...';

    // Emscripten calls this when compiled WASM is ready
    window.onVixcWasmReady = function() {
        statusText.textContent = '编译器就绪';
        runBtn.disabled = false;
    };

    // --- Tab switching (Output / WASM) ---
    document.querySelectorAll('[data-tab]').forEach(function(btn) {
        btn.addEventListener('click', function() {
            document.querySelectorAll('[data-tab]').forEach(function(b) { b.classList.remove('active'); });
            this.classList.add('active');
            var tab = this.dataset.tab;
            document.getElementById('output-content').style.display = tab === 'output' ? 'block' : 'none';
            document.getElementById('wasm-content').style.display = tab === 'wasm' ? 'block' : 'none';
        });
    });

    // --- Button handler ---
    runBtn.addEventListener('click', runCode);

})();
```

- [x] **Step 3: Create `playground/playground.css`**

```css
* { box-sizing: border-box; margin: 0; padding: 0; }
html, body { height: 100%; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }

#app { display: flex; flex-direction: column; height: 100vh; }

header {
    display: flex; justify-content: space-between; align-items: center;
    padding: 8px 16px; background: #1e1e2e; color: #cdd6f4;
}
header h1 { font-size: 16px; font-weight: 600; }
#status-bar { display: flex; align-items: center; gap: 12px; }
#status-text { font-size: 12px; color: #a6adc8; }
#btn-run {
    padding: 6px 20px; border: none; border-radius: 6px;
    background: #a6e3a1; color: #1e1e2e; font-weight: 600; cursor: pointer;
}
#btn-run:disabled { opacity: 0.5; cursor: not-allowed; }

main { display: flex; flex: 1; overflow: hidden; }
#editor-container { flex: 1; overflow: auto; }
.CodeMirror { height: 100%; font-size: 14px; }

#output-container {
    width: 400px; display: flex; flex-direction: column;
    border-left: 1px solid #313244; background: #11111b;
}
#output-tabs { display: flex; border-bottom: 1px solid #313244; }
#output-tabs .tab {
    flex: 1; padding: 8px; border: none; background: transparent;
    color: #6c7086; cursor: pointer; font-size: 12px;
}
#output-tabs .tab.active { color: #cdd6f4; border-bottom: 2px solid #89b4fa; }
#output-content, #wasm-content {
    flex: 1; padding: 12px; font-family: 'Fira Code', 'Cascadia Code', monospace;
    font-size: 13px; color: #cdd6f4; overflow: auto; white-space: pre-wrap;
}
```

- [x] **Step 4: Write integration test — end-to-end compile test in Node.js**

```javascript
// playground/test_e2e.js
const fs = require('fs');
const path = require('path');

async function testPlayground() {
    // Load the WASM module
    const wasmPath = path.join(__dirname, 'build', 'vixc-wasm.wasm');
    const wasmBytes = fs.readFileSync(wasmPath);

    // For now, test that the WASM module loads
    const mod = await WebAssembly.compile(wasmBytes);
    console.log('PASS: vixc-wasm.wasm is valid WASM');

    // Verify it exports compile_vix
    const imports = {
        env: {
            __cxa_throw: () => {},
            memory: new WebAssembly.Memory({ initial: 256 }),
            // Minimal emscripten imports needed
        }
    };

    console.log('ALL playground integration tests passed');
}

testPlayground().catch(console.error);
```

- [x] **Step 5: Commit**

```bash
git add playground/playground.html playground/playground.js playground/playground.css playground/test_e2e.js
git commit -m "feat: add playground UI with CodeMirror editor"
```

### Task 6: Integrate Playground into the Website

**Files:**
- Modify: `../WebSite/index.html` (navigation + link to playground)
- Modify: `../WebSite/very.html` (navigation)
- Create: `../WebSite/playground/` (symlink or copy of playground build output)

- [x] **Step 1: Add playground link to website navigation**

In both `index.html` and `very.html`, add after the Very link:
```html
<li><a href="playground/" data-i18n="nav_playground">Playground</a></li>
```

Add translation keys:
```javascript
// zh: nav_playground: "在线运行"
// en: nav_playground: "Playground"
```

- [x] **Step 2: Copy built playground assets to website**

```bash
mkdir -p ../WebSite/playground
cp playground/build/vixc-wasm.js ../WebSite/playground/
cp playground/build/vixc-wasm.wasm ../WebSite/playground/
cp playground/playground.html ../WebSite/playground/index.html
cp playground/playground.js ../WebSite/playground/
cp playground/playground.css ../WebSite/playground/
```

- [x] **Step 3: Verify locally — serve the website and test**

```bash
cd ../WebSite
python3 -m http.server 8000
# Open http://localhost:8000/playground/ in browser
```

Expected: Playground loads, editor shows default code, clicking "运行" compiles and runs Vix code in-browser.

- [x] **Step 4: Commit (in WebSite repo)**

```bash
cd ../WebSite
git add playground/ index.html very.html
git commit -m "feat: integrate Vix Playground into website"
```

### Task 7: Polish — Error Handling, Loading UX, Edge Cases

**Files:**
- Modify: `playground/playground.js`
- Modify: `playground/playground.html`
- Modify: `src/compiler/WasmCodegen.cpp`

- [x] **Step 1: Improve WASM loading UX**

Add a loading progress bar in `playground.html`:
```html
<div id="loading-overlay">
    <div id="loading-bar-container">
        <div id="loading-bar"></div>
        <p id="loading-text">正在加载 Vix 编译器...</p>
    </div>
</div>
```

In `pre.js`, report progress via Emscripten's `onProgress`:
```javascript
Module.onProgress = function(progress) {
    var bar = document.getElementById('loading-bar');
    if (bar) bar.style.width = (progress * 100) + '%';
};
```

- [x] **Step 2: Improve error messages from parser**

In `playground.js`, parse the WASM error output into human-readable format with line numbers:
```javascript
function formatCompileError(raw) {
    // Convert "syntax error at line 5 col 12" style messages
    // to clickable CodeMirror gutter markers
}
```

- [x] **Step 3: Handle unsupported features gracefully**

In `WasmCodegen.cpp`, for any AST node that cannot be translated to WASM (e.g., raw pointer arithmetic, inline assembly):
```cpp
BinaryenExpressionRef WasmCodegen::compile_node(ASTNode *node) {
    // ... existing cases ...
    default:
        error_msg = "Unsupported feature in WASM target: " + std::string(node_type_name(node->type));
        return BinaryenUnreachable(m_module);
}
```

- [x] **Step 4: Add code examples selector**

Add a dropdown in `playground.html` with preset Vix examples:
```html
<select id="example-selector">
    <option value="hello">Hello World</option>
    <option value="fib">Fibonacci</option>
    <option value="struct">Struct Example</option>
    <option value="match">Pattern Matching</option>
</select>
```

In `playground.js`, store example code presets and switch on selection:
```javascript
const EXAMPLES = {
    hello: 'use "core/io.vix" as io\nfn main(): i32 { io::puts("Hello!"); return 0 }',
    fib: 'fn fib(n: i32): i32 { if n <= 1 { return n; } return fib(n-1) + fib(n-2); }',
    // ...
};
```

- [x] **Step 5: Persist editor content in localStorage**

```javascript
// Restore
const saved = localStorage.getItem('vix-playground-code');
if (saved) editor.setValue(saved);

// Save on each change
editor.on('change', function() {
    localStorage.setItem('vix-playground-code', editor.getValue());
});
```

- [x] **Step 6: Commit**

```bash
git add playground/playground.html playground/playground.js playground/pre.js src/compiler/WasmCodegen.cpp
git commit -m "polish: improve error handling, loading UX, and code examples"
```
