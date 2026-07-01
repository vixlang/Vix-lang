cmake_minimum_required(VERSION 3.20)
project(vixc LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Binaryen requires C++20; set globally for simplicity
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(BISON 3.0 REQUIRED)
find_package(FLEX 2.6 REQUIRED)

if(APPLE)
    set(_brew_llvm_prefix "")
    set(_brew_lld_prefix "")
    foreach(_candidate "llvm" "llvm@20" "llvm@19" "llvm@18")
        execute_process(
            COMMAND brew --prefix "${_candidate}"
            OUTPUT_VARIABLE _prefix
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _brew_rc
        )
        if(_brew_rc EQUAL 0 AND EXISTS "${_prefix}/lib/cmake/llvm/LLVMConfig.cmake")
            set(_brew_llvm_prefix "${_prefix}")
            break()
        endif()
    endforeach()
    execute_process(
        COMMAND brew --prefix lld
        OUTPUT_VARIABLE _prefix
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _brew_rc
    )
    if(_brew_rc EQUAL 0 AND EXISTS "${_prefix}/lib/cmake/lld/LLDConfig.cmake")
        set(_brew_lld_prefix "${_prefix}")
    endif()
    if(_brew_llvm_prefix)
        list(PREPEND CMAKE_PREFIX_PATH "${_brew_llvm_prefix}")
        message(STATUS "Homebrew LLVM prefix: ${_brew_llvm_prefix}")
    else()
        message(WARNING "No Homebrew LLVM found. Install via: brew install llvm")
    endif()
    if(_brew_lld_prefix)
        list(PREPEND CMAKE_PREFIX_PATH "${_brew_lld_prefix}")
        message(STATUS "Homebrew LLD prefix: ${_brew_lld_prefix}")
    endif()
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

get_filename_component(_llvm_cmake_dir "${LLVM_DIR}" ABSOLUTE)
set(_lld_cmake_candidates
    "${_llvm_cmake_dir}/../lld"
    "${_llvm_cmake_dir}/../../lld"
    "${_llvm_cmake_dir}/../../../lib/cmake/lld"
    "${_llvm_cmake_dir}/../lib/cmake/lld"
)
if(APPLE AND _brew_llvm_prefix)
    list(APPEND _lld_cmake_candidates "${_brew_llvm_prefix}/lib/cmake/lld")
endif()
if(APPLE AND _brew_lld_prefix)
    list(APPEND _lld_cmake_candidates "${_brew_lld_prefix}/lib/cmake/lld")
endif()
if(WIN32 AND CMAKE_PREFIX_PATH)
    foreach(_pp IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND _lld_cmake_candidates "${_pp}/lib/cmake/lld")
    endforeach()
endif()

set(_lld_found OFF)
foreach(_lld_dir IN LISTS _lld_cmake_candidates)
    get_filename_component(_lld_dir "${_lld_dir}" ABSOLUTE)
    if(EXISTS "${_lld_dir}/LLDConfig.cmake")
        find_package(LLD CONFIG PATHS "${_lld_dir}" NO_DEFAULT_PATH)
        if(LLD_FOUND)
            set(_lld_found ON)
            message(STATUS "Found LLD at ${_lld_dir}")
            break()
        endif()
    endif()
endforeach()

if(NOT _lld_found)
    find_package(LLD CONFIG)
    if(NOT LLD_FOUND)
        message(FATAL_ERROR "LLD not found. Please install LLVM/LLD or set LLD_DIR to the LLD cmake directory.")
    endif()
endif()

include_directories(${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/parser)

BISON_TARGET(vix_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/parser.y
    ${CMAKE_CURRENT_BINARY_DIR}/parser/parser.tab.c
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser/parser.tab.h
)

FLEX_TARGET(vix_lexer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/lexer.l
    ${CMAKE_CURRENT_BINARY_DIR}/parser/lex.yy.c
)
ADD_FLEX_BISON_DEPENDENCY(vix_lexer vix_parser)
set(C_SOURCES
    src/main.c
    src/ast/ast.c
    src/ast/typeinfer.c
    src/semantic/semantic.c
    src/utils/error.c
    ${BISON_vix_parser_OUTPUTS}
    ${FLEX_vix_lexer_OUTPUTS}
)
set(CXX_SOURCES
    src/compiler/Codegen.cpp
    src/compiler/Types.cpp
    src/compiler/Attrs.cpp
    src/compiler/Exprs.cpp
    src/compiler/Stmts.cpp
    src/compiler/Funcs.cpp
    src/compiler/Structs.cpp
    src/compiler/Arrays.cpp
    src/compiler/Passes.cpp
    src/compiler/Llc/Llc.cpp
    src/compiler/Linker/Linker.cpp
    src/Typeck/Typeck.cpp
    src/Typeck/TypeckInfer.cpp
    src/Typeck/LayOut.cpp
    src/Ownership/Ownership.cpp
)

if(WIN32)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/parser/lex.yy.c
        PROPERTIES COMPILE_DEFINITIONS "YY_NO_UNISTD_H")
endif()

add_executable(vixc ${C_SOURCES} ${CXX_SOURCES})

target_include_directories(vixc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler
    ${CMAKE_CURRENT_BINARY_DIR}/parser
)
target_compile_options(vixc PRIVATE
    $<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:MSVC>>:/W4>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/W4 /EHsc>
    $<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<C_COMPILER_ID:MSVC>>>:-Wall -Wextra -fstack-protector-strong>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Wall -Wextra -fstack-protector-strong -fexceptions>
)

if(NOT WIN32)
    target_compile_definitions(vixc PRIVATE
        _POSIX_C_SOURCE=200809L
        _XOPEN_SOURCE=700
    )
endif()
if(WIN32)
    # 移除 LLVMWindowsManifest 对动态 LibXml2::LibXml2 的依赖
    if(TARGET LLVMWindowsManifest)
        get_target_property(_iface_libs LLVMWindowsManifest INTERFACE_LINK_LIBRARIES)
        if(_iface_libs)
            list(REMOVE_ITEM _iface_libs LibXml2::LibXml2)
            set_target_properties(LLVMWindowsManifest PROPERTIES
                INTERFACE_LINK_LIBRARIES "${_iface_libs}")
        endif()
    endif()

    # 探测 vcpkg 静态 triplet 路径
    set(_vcpkg_static "$ENV{VCPKG_ROOT}/installed/x64-windows-static")
    if(NOT EXISTS "${_vcpkg_static}")
        set(_vcpkg_static "E:/spj/vcpkg/installed/x64-windows-static")
    endif()

    # 静态库路径
    set(_libxml2_static "${_vcpkg_static}/lib/libxml2.lib")
    set(_zlib_static    "${_vcpkg_static}/lib/zlib.lib")
    set(_iconv_static   "${_vcpkg_static}/lib/iconv.lib")

    if(NOT EXISTS "${_libxml2_static}")
        message(FATAL_ERROR "Static libxml2 not found. Run: vcpkg install libxml2:x64-windows-static")
    endif()

    target_link_libraries(vixc PRIVATE
        "${_libxml2_static}"
        "${_zlib_static}"
        "${_iconv_static}"
    )
    target_include_directories(vixc PRIVATE
        "${_vcpkg_static}/include/libxml2"
    )

    # libxml2 的 dict.c 需要 BCryptGenRandom
    target_link_libraries(vixc PRIVATE bcrypt.lib)

    message(STATUS "Linked static libxml2: ${_libxml2_static}")
    message(STATUS "Linked static zlib:    ${_zlib_static}")
    message(STATUS "Linked static iconv:   ${_iconv_static}")
endif()
if(TARGET LLVM)
    target_link_libraries(vixc PRIVATE LLVM)
else()
    llvm_map_components_to_libnames(LLVM_LIBS
        core support passes irreader target
        x86asmparser x86codegen x86desc x86disassembler x86info
        aarch64asmparser aarch64codegen aarch64desc aarch64disassembler aarch64info
        armasmparser armcodegen armdesc armdisassembler arminfo
        webassemblyasmparser webassemblycodegen webassemblydesc webassemblydisassembler webassemblyinfo
        mc mcparser mcdisassembler bitreader bitwriter
        linker instrumentation ipo codegen
    )
    target_link_libraries(vixc PRIVATE ${LLVM_LIBS})
endif()
foreach(_lld_target IN ITEMS lldELF lldMachO lldCOFF lldWasm lldMinGW lldCommon)
    if(TARGET ${_lld_target})
        target_link_libraries(vixc PRIVATE ${_lld_target})
    endif()
endforeach()

foreach(lib IN LISTS LLVM_SYSTEM_LIBS)
    target_link_libraries(vixc PRIVATE ${lib})
endforeach()

install(TARGETS vixc RUNTIME DESTINATION bin)

# ── Binaryen (for WASM codegen) ──────────────────────────────────
option(BUILD_WASM_CODEGEN "Build WASM codegen support (requires Binaryen)" OFF)
if(EMSCRIPTEN OR BUILD_WASM_CODEGEN)
    if(EMSCRIPTEN)
        # Emscripten: use submodule source
        set(BINARYEN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/binaryen")
        if(EXISTS "${BINARYEN_SRC_DIR}/CMakeLists.txt")
            set(BINARYEN_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/binaryen")
            add_subdirectory("${BINARYEN_SRC_DIR}" "${BINARYEN_BUILD_DIR}" EXCLUDE_FROM_ALL)
        endif()
    else()
        # Native: build from submodule source as static lib, or fall back to system
        set(BINARYEN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/binaryen")
        if(EXISTS "${BINARYEN_SRC_DIR}/CMakeLists.txt")
            set(BINARYEN_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/binaryen_native")
            set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
            set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
            set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
            add_subdirectory("${BINARYEN_SRC_DIR}" "${BINARYEN_BUILD_DIR}" EXCLUDE_FROM_ALL)
            set_target_properties(binaryen PROPERTIES EXCLUDE_FROM_ALL TRUE)
            target_include_directories(binaryen INTERFACE "${BINARYEN_SRC_DIR}/src")
            message(STATUS "Building Binaryen from source at ${BINARYEN_SRC_DIR}")
        else()
            # Fallback: use system-installed Binaryen
            find_library(BINARYEN_LIBRARY NAMES binaryen libbinaryen)
            find_path(BINARYEN_INCLUDE_DIR binaryen-c.h)
            if(BINARYEN_LIBRARY AND BINARYEN_INCLUDE_DIR)
                message(STATUS "Found Binaryen: ${BINARYEN_LIBRARY}")
                add_library(binaryen UNKNOWN IMPORTED)
                set_target_properties(binaryen PROPERTIES
                    IMPORTED_LOCATION "${BINARYEN_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${BINARYEN_INCLUDE_DIR}"
                )
            else()
                message(WARNING "Binaryen not found. Install mingw-w64-ucrt-x86_64-binaryen or set -DBUILD_WASM_CODEGEN=OFF")
            endif()
        endif()
    endif()
endif()

# ── Frontend test (no LLVM dependency) ────────────────────────────
option(BUILD_FRONTEND_TEST "Build frontend library test" OFF)
if(BUILD_FRONTEND_TEST)
    set(VIXC_FRONTEND_SOURCES
        playground/vixc_frontend.c
        tests/test_frontend.c
        src/frontend_globals.c
        src/ast/ast.c
        src/ast/typeinfer.c
        src/semantic/semantic.c
        src/utils/error.c
        ${BISON_vix_parser_OUTPUTS}
        ${FLEX_vix_lexer_OUTPUTS}
        src/Typeck/Typeck.cpp
        src/Typeck/TypeckInfer.cpp
        src/Typeck/LayOut.cpp
        src/Ownership/Ownership.cpp
    )
    add_executable(test_frontend ${VIXC_FRONTEND_SOURCES})
    target_include_directories(test_frontend PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/parser
    )
    target_compile_definitions(test_frontend PRIVATE VIXC_FRONTEND_ONLY)
    target_compile_options(test_frontend PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<C_COMPILER_ID:MSVC>>>:-Wall -Wextra>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Wall -Wextra>
    )
    if(NOT WIN32)
        target_compile_definitions(test_frontend PRIVATE
            _POSIX_C_SOURCE=200809L
            _XOPEN_SOURCE=700
        )
    endif()
    if(FLEX_LIBRARIES)
        target_link_libraries(test_frontend PRIVATE ${FLEX_LIBRARIES})
    endif()
endif()

# ── WASM codegen test ──────────────────────────────────────────────
option(BUILD_WASM_TEST "Build WASM codegen test" OFF)
if(BUILD_WASM_TEST AND TARGET binaryen)
    set(WASM_TEST_SOURCES
        tests/test_wasm_codegen.cpp
        playground/vixc_frontend.c
        src/frontend_globals.c
        src/compiler/WasmCodegen.cpp
        src/compiler/WasmTypeMap.cpp
        src/ast/ast.c
        src/ast/typeinfer.c
        src/semantic/semantic.c
        src/utils/error.c
        ${BISON_vix_parser_OUTPUTS}
        ${FLEX_vix_lexer_OUTPUTS}
        src/Typeck/Typeck.cpp
        src/Typeck/TypeckInfer.cpp
        src/Typeck/LayOut.cpp
        src/Ownership/Ownership.cpp
    )
    add_executable(test_wasm_codegen ${WASM_TEST_SOURCES})
    target_include_directories(test_wasm_codegen PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler
        ${CMAKE_CURRENT_BINARY_DIR}/parser
    )
    target_compile_definitions(test_wasm_codegen PRIVATE VIXC_FRONTEND_ONLY)
    target_compile_options(test_wasm_codegen PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<C_COMPILER_ID:MSVC>>>:-Wall -Wextra>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Wall -Wextra>
    )
    if(NOT WIN32)
        target_compile_definitions(test_wasm_codegen PRIVATE
            _POSIX_C_SOURCE=200809L
            _XOPEN_SOURCE=700
        )
    endif()
    target_link_libraries(test_wasm_codegen PRIVATE binaryen)
    if(MINGW)
        # Ninja for MinGW adds -Wl,--out-implib automatically; remove it
        set_target_properties(test_wasm_codegen PROPERTIES ENABLE_EXPORTS OFF)
    endif()
endif()
