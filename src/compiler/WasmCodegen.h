#ifndef VIX_WASM_CODEGEN_H
#define VIX_WASM_CODEGEN_H

#include "ast.h"
#include "binaryen-c.h"
#include "WasmTypeMap.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

class WasmCodegen {
public:
    WasmCodegen();
    ~WasmCodegen();

    bool emit(ASTNode *root, std::vector<uint8_t> &out_bytes, std::string &error_msg);

private:
    BinaryenModuleRef m_module;
    std::string m_error;

    struct StringEntry {
        std::vector<char> data;
        uint32_t offset;
    };

    struct FuncInfo {
        BinaryenFunctionRef func_ref;
        std::unordered_map<std::string, uint32_t> local_indices;
        uint32_t next_local;
        uint32_t param_count;
        std::vector<uintptr_t> param_types;
        uintptr_t return_type;
    };

    std::unordered_map<std::string, FuncInfo> m_functions;
    FuncInfo *m_current_func;
    bool m_has_error;

    std::vector<StringEntry> m_strings;
    uint32_t m_string_offset;

    std::vector<std::string> m_break_labels;
    std::vector<std::string> m_continue_labels;

    uint32_t m_heap_offset;

    std::unordered_map<std::string, WasmStructLayout> m_struct_layouts;

    void register_struct_layout(ASTNode *node);
    const WasmFieldLayout *find_field_layout(const std::string &struct_name, const std::string &field_name) const;

    uint32_t alloc_bytes(uint32_t size, uint32_t align);
    uintptr_t emit_i32_load(uintptr_t addr);
    uintptr_t emit_i32_store(uintptr_t addr, uintptr_t value);
    uintptr_t compile_array_literal(ASTNode *node);
    uintptr_t compile_index(ASTNode *node);
    uintptr_t compile_index_assign(ASTNode *assign_node);
    uintptr_t emit_array_length(uintptr_t array_ptr);
    uintptr_t compile_struct_literal(ASTNode *node);
    uintptr_t compile_member_assign(ASTNode *assign_node);

    void add_imports();
    void register_function(ASTNode *node);
    void compile_function_body(ASTNode *node);
    void bind_param_locals(ASTNode *params);

    uintptr_t compile_node(ASTNode *node);
    uintptr_t compile_block(ASTNode *stmt_list);
    uintptr_t compile_if(ASTNode *if_node);
    uintptr_t compile_while(ASTNode *while_node);
    uintptr_t compile_for(ASTNode *for_node);
    uintptr_t compile_break(ASTNode *node);
    uintptr_t compile_continue(ASTNode *node);
    uintptr_t compile_binary_op(ASTNode *op_node);
    uintptr_t compile_call(ASTNode *call_node);
    uintptr_t compile_print(ASTNode *print_node);
    uintptr_t compile_ident(ASTNode *ident_node);
    uintptr_t compile_return(ASTNode *ret_node);
    uintptr_t compile_member_access(ASTNode *node);
    uintptr_t compile_assign(ASTNode *assign_node);
    uintptr_t compile_var_decl(ASTNode *decl_node, ASTNode *init_expr);

    uint32_t get_or_create_local(const char *name, uintptr_t wasm_type);

    void set_error(const std::string &msg);
};

#endif
