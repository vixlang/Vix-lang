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

WasmTypeInfo map_vix_type_to_wasm(const Type *type);

#endif
