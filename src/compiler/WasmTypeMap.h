#ifndef VIX_WASM_TYPEMAP_H
#define VIX_WASM_TYPEMAP_H

#include "type.h"
#include <cstdint>

struct WasmTypeInfo {
    uintptr_t val_type; // BinaryenType
    int32_t wasm_memory_size;
    bool is_struct;
    bool is_pointer_like;
};

struct WasmFieldLayout {
    std::string name;
    uint32_t offset;
    uint32_t size;
};

struct WasmStructLayout {
    uint32_t size;
    std::vector<WasmFieldLayout> fields;
};

WasmTypeInfo map_vix_type_to_wasm(const Type *type);

#endif
