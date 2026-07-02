cmake_minimum_required(VERSION 3.20)
project(vixc-wasm LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

if(EMSCRIPTEN)
    find_package(BISON 3.0 REQUIRED)
    find_package(FLEX 2.6 REQUIRED)

    # Generate parser from .y/.l files
    set(BISON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/parser")
    set(BISON_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/parser/parser.tab.c")
    set(FLEX_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/parser/lex.yy.c")

    bison_target(vix_parser ${BISON_DIR}/parser.y ${BISON_OUTPUT})
    flex_target(vix_lexer ${BISON_DIR}/lexer.l ${FLEX_OUTPUT})
    add_flex_bison_dependency(vix_lexer vix_parser)

    set(VIX_WASM_SOURCES
        vixc-wasm.cpp
        vixc_frontend.c
        ../src/frontend_globals.c
        ../src/ast/ast.c
        ../src/ast/typeinfer.c
        ../src/semantic/semantic.c
        ../src/utils/error.c
        ../src/compiler/WasmCodegen.cpp
        ../src/compiler/WasmTypeMap.cpp
        ../src/Typeck/Typeck.cpp
        ../src/Typeck/TypeckInfer.cpp
        ../src/Typeck/LayOut.cpp
        ../src/Ownership/Ownership.cpp
        ${BISON_OUTPUT}
        ${FLEX_OUTPUT}
    )

    set(BINARYEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/binaryen")

    if(EXISTS "${BINARYEN_DIR}/CMakeLists.txt")
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_TOOLS OFF)
        set(BUILD_TESTS OFF)
        add_subdirectory("${BINARYEN_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/binaryen" EXCLUDE_FROM_ALL)
    endif()

    add_executable(vixc-wasm ${VIX_WASM_SOURCES})
    target_include_directories(vixc-wasm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/compiler
        ${BINARYEN_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/parser
    )

    target_compile_definitions(vixc-wasm PRIVATE VIXC_FRONTEND_ONLY)
    target_compile_options(vixc-wasm PRIVATE -frtti -fexceptions)

    target_link_libraries(vixc-wasm PRIVATE binaryen)

    set_target_properties(vixc-wasm PROPERTIES
        LINK_FLAGS "-s WASM=1 -s ALLOW_MEMORY_GROWTH=1 -s DISABLE_EXCEPTION_CATCHING=0 -s \"EXPORTED_FUNCTIONS=['_compile_vix', '_free_wasm_result', '_malloc', '_free']\" -s \"EXPORTED_RUNTIME_METHODS=['ccall', 'getValue', 'setValue', 'UTF8ToString', 'stringToUTF8']\" -O2 --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/pre.js"
    )
endif()
