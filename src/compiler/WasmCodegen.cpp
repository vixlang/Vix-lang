#include "WasmCodegen.h"
#include "WasmTypeMap.h"
#include "binaryen-c.h"
#include <cstring>

WasmCodegen::WasmCodegen()
    : m_module(nullptr), m_current_func(nullptr), m_has_error(false),
      m_string_offset(16), m_heap_offset(4096) {}

WasmCodegen::~WasmCodegen() {
    if (m_module) BinaryenModuleDispose(m_module);
}

void WasmCodegen::set_error(const std::string &msg) {
    if (!m_has_error) {
        m_error = msg;
        m_has_error = true;
    }
}

void WasmCodegen::add_imports() {
    BinaryenType one_i32[] = {BinaryenTypeInt32()};
    BinaryenType void_t = BinaryenTypeNone();

    BinaryenAddFunctionImport(m_module, "vix_putchar", "env", "vix_putchar",
                              BinaryenTypeCreate(one_i32, 1), void_t);
    BinaryenAddFunctionImport(m_module, "vix_puts", "env", "vix_puts",
                              BinaryenTypeCreate(one_i32, 1), void_t);
    BinaryenAddFunctionImport(m_module, "vix_print_i32", "env", "vix_print_i32",
                              BinaryenTypeCreate(one_i32, 1), void_t);
    BinaryenAddFunctionImport(m_module, "vix_exit", "env", "vix_exit",
                              BinaryenTypeCreate(one_i32, 1), void_t);
}

void WasmCodegen::register_function(ASTNode *node) {
    if (!node || node->type != AST_FUNCTION || !node->data.function.name) return;

    std::string name(node->data.function.name);
    std::vector<uintptr_t> param_types;
    ASTNode *params = node->data.function.params;
    if (params && params->type == AST_EXPRESSION_LIST) {
        for (int i = 0; i < params->data.expression_list.expression_count; i++) {
            ASTNode *p = params->data.expression_list.expressions[i];
            if (p && p->type == AST_ASSIGN && p->data.assign.left) {
                param_types.push_back(BinaryenTypeInt32());
            } else {
                param_types.push_back(BinaryenTypeInt32());
            }
        }
    }

    uintptr_t ret_type = BinaryenTypeInt32();
    if (node->data.function.return_type) {
        ASTNode *rt = node->data.function.return_type;
        if (rt->type == AST_TYPE_VOID) {
            ret_type = BinaryenTypeNone();
        }
    }

    FuncInfo info;
    info.func_ref = 0;
    info.next_local = param_types.size();
    info.param_count = param_types.size();
    info.param_types = param_types;
    info.return_type = ret_type;

    m_functions[name] = info;
}

uint32_t WasmCodegen::get_or_create_local(const char *name, uintptr_t wasm_type) {
    if (!m_current_func) return 0;
    auto it = m_current_func->local_indices.find(name);
    if (it != m_current_func->local_indices.end()) {
        return it->second;
    }
    uint32_t idx = m_current_func->next_local++;
    m_current_func->local_indices[name] = idx;
    return idx;
}

void WasmCodegen::bind_param_locals(ASTNode *params) {
    if (!m_current_func || !params || params->type != AST_EXPRESSION_LIST) return;
    for (int i = 0; i < params->data.expression_list.expression_count; i++) {
        ASTNode *p = params->data.expression_list.expressions[i];
        if (!p || p->type != AST_ASSIGN || !p->data.assign.left) continue;
        ASTNode *left = p->data.assign.left;
        if (left->type != AST_IDENTIFIER || !left->data.identifier.name) continue;
        m_current_func->local_indices[left->data.identifier.name] = (uint32_t)i;
    }
}

void WasmCodegen::compile_function_body(ASTNode *node) {
    if (!node || node->type != AST_FUNCTION || !node->data.function.name) return;

    std::string name(node->data.function.name);
    auto fit = m_functions.find(name);
    if (fit == m_functions.end()) return;

    m_current_func = &fit->second;
    bind_param_locals(node->data.function.params);

    ASTNode *body = node->data.function.body;
    std::vector<uintptr_t> exprs;

    if (body && body->type == AST_PROGRAM) {
        for (int i = 0; i < body->data.program.statement_count; i++) {
            ASTNode *stmt = body->data.program.statements[i];
            if (stmt) {
                uintptr_t expr = compile_node(stmt);
                if (expr) exprs.push_back(expr);
            }
        }
    } else if (body) {
        uintptr_t expr = compile_node(body);
        if (expr) exprs.push_back(expr);
    }

    uintptr_t block_expr;
    if (exprs.empty()) {
        block_expr = (uintptr_t)BinaryenNop(m_module);
    } else if (exprs.size() == 1) {
        block_expr = exprs[0];
    } else {
        block_expr = (uintptr_t)BinaryenBlock(m_module, nullptr,
                                              (BinaryenExpressionRef*)exprs.data(),
                                              exprs.size(), BinaryenTypeAuto());
    }

    uint32_t local_count = m_current_func->next_local - m_current_func->param_count;
    std::vector<BinaryenType> local_types(local_count, BinaryenTypeInt32());

    m_current_func->func_ref = BinaryenAddFunction(
        m_module, name.c_str(),
        BinaryenTypeCreate(m_current_func->param_types.data(), m_current_func->param_types.size()),
        m_current_func->return_type,
        local_types.data(), local_count,
        (BinaryenExpressionRef)block_expr);

    m_current_func = nullptr;
}

uint32_t WasmCodegen::alloc_bytes(uint32_t size, uint32_t align) {
    uint32_t base = (m_heap_offset + align - 1) & ~(align - 1);
    m_heap_offset = base + size;
    return base;
}

uintptr_t WasmCodegen::emit_i32_load(uintptr_t addr) {
    return (uintptr_t)BinaryenLoad(m_module, 4, true, 0, 4, BinaryenTypeInt32(),
                                    (BinaryenExpressionRef)addr, "0");
}

uintptr_t WasmCodegen::emit_i32_store(uintptr_t addr, uintptr_t value) {
    return (uintptr_t)BinaryenStore(m_module, 4, 0, 4, (BinaryenExpressionRef)addr,
                                     (BinaryenExpressionRef)value, BinaryenTypeInt32(), "0");
}

uintptr_t WasmCodegen::emit_array_length(uintptr_t array_ptr) {
    return emit_i32_load(array_ptr);
}

uintptr_t WasmCodegen::compile_array_literal(ASTNode *node) {
    int count = node->data.expression_list.expression_count;
    uint32_t total = 8 + (uint32_t)count * 4;
    uint32_t base = alloc_bytes(total, 4);

    std::vector<BinaryenExpressionRef> exprs;
    exprs.push_back((BinaryenExpressionRef)emit_i32_store(
        (uintptr_t)BinaryenConst(m_module, BinaryenLiteralInt32((int32_t)base)),
        (uintptr_t)BinaryenConst(m_module, BinaryenLiteralInt32(count))));
    exprs.push_back((BinaryenExpressionRef)emit_i32_store(
        (uintptr_t)BinaryenConst(m_module, BinaryenLiteralInt32((int32_t)(base + 4))),
        (uintptr_t)BinaryenConst(m_module, BinaryenLiteralInt32(4))));

    for (int i = 0; i < count; i++) {
        uintptr_t value = compile_node(node->data.expression_list.expressions[i]);
        exprs.push_back((BinaryenExpressionRef)emit_i32_store(
            (uintptr_t)BinaryenConst(m_module, BinaryenLiteralInt32((int32_t)(base + 8 + i * 4))),
            value));
    }

    exprs.push_back(BinaryenConst(m_module, BinaryenLiteralInt32((int32_t)base)));
    return (uintptr_t)BinaryenBlock(m_module, nullptr, exprs.data(), exprs.size(), BinaryenTypeInt32());
}

uintptr_t WasmCodegen::compile_index(ASTNode *node) {
    uintptr_t base = compile_node(node->data.index.target);
    uintptr_t idx = compile_node(node->data.index.index);
    uintptr_t dataAddr = (uintptr_t)BinaryenBinary(
        m_module,
        BinaryenAddInt32(),
        (BinaryenExpressionRef)base,
        BinaryenConst(m_module, BinaryenLiteralInt32(8)));
    uintptr_t byteOffset = (uintptr_t)BinaryenBinary(
        m_module,
        BinaryenShlInt32(),
        (BinaryenExpressionRef)idx,
        BinaryenConst(m_module, BinaryenLiteralInt32(2)));
    uintptr_t addr = (uintptr_t)BinaryenBinary(
        m_module,
        BinaryenAddInt32(),
        (BinaryenExpressionRef)dataAddr,
        (BinaryenExpressionRef)byteOffset);
    return emit_i32_load(addr);
}

uintptr_t WasmCodegen::compile_index_assign(ASTNode *assign_node) {
    ASTNode *target = assign_node->data.assign.left;
    uintptr_t val = compile_node(assign_node->data.assign.right);
    uintptr_t base = compile_node(target->data.index.target);
    uintptr_t idx = compile_node(target->data.index.index);
    uintptr_t dataAddr = (uintptr_t)BinaryenBinary(
        m_module,
        BinaryenAddInt32(),
        (BinaryenExpressionRef)base,
        BinaryenConst(m_module, BinaryenLiteralInt32(8)));
    uintptr_t byteOffset = (uintptr_t)BinaryenBinary(
        m_module,
        BinaryenShlInt32(),
        (BinaryenExpressionRef)idx,
        BinaryenConst(m_module, BinaryenLiteralInt32(2)));
    uintptr_t addr = (uintptr_t)BinaryenBinary(
        m_module,
        BinaryenAddInt32(),
        (BinaryenExpressionRef)dataAddr,
        (BinaryenExpressionRef)byteOffset);
    return emit_i32_store(addr, val);
}

void WasmCodegen::register_struct_layout(ASTNode *node) {
    if (!node || node->type != AST_STRUCT_DEF || !node->data.struct_def.name) return;
    WasmStructLayout layout = {};
    uint32_t offset = 0;
    ASTNode *fields = node->data.struct_def.fields;
    if (fields && fields->type == AST_EXPRESSION_LIST) {
        for (int i = 0; i < fields->data.expression_list.expression_count; i++) {
            ASTNode *f = fields->data.expression_list.expressions[i];
            if (!f || f->type != AST_ASSIGN || !f->data.assign.left) continue;
            ASTNode *left = f->data.assign.left;
            if (left->type != AST_IDENTIFIER || !left->data.identifier.name) continue;
            layout.fields.push_back({left->data.identifier.name, offset, 4});
            offset += 4;
        }
    }
    layout.size = offset;
    m_struct_layouts[node->data.struct_def.name] = layout;
}

const WasmFieldLayout *WasmCodegen::find_field_layout(const std::string &struct_name, const std::string &field_name) const {
    auto it = m_struct_layouts.find(struct_name);
    if (it == m_struct_layouts.end()) return nullptr;
    for (const auto &f : it->second.fields) {
        if (f.name == field_name) return &f;
    }
    return nullptr;
}

uintptr_t WasmCodegen::compile_struct_literal(ASTNode *node) {
    ASTNode *type_name = node->data.struct_literal.type_name;
    if (!type_name || type_name->type != AST_IDENTIFIER || !type_name->data.identifier.name) {
        return (uintptr_t)BinaryenNop(m_module);
    }
    std::string structName(type_name->data.identifier.name);
    auto it = m_struct_layouts.find(structName);
    if (it == m_struct_layouts.end()) return (uintptr_t)BinaryenNop(m_module);

    uint32_t base = alloc_bytes(it->second.size, 4);
    std::vector<BinaryenExpressionRef> exprs;
    ASTNode *fields = node->data.struct_literal.fields;
    if (fields && fields->type == AST_EXPRESSION_LIST) {
        for (int i = 0; i < fields->data.expression_list.expression_count; i++) {
            ASTNode *f = fields->data.expression_list.expressions[i];
            if (!f || f->type != AST_ASSIGN || !f->data.assign.left || !f->data.assign.right) continue;
            ASTNode *left = f->data.assign.left;
            if (left->type != AST_IDENTIFIER || !left->data.identifier.name) continue;
            const WasmFieldLayout *layout = find_field_layout(structName, left->data.identifier.name);
            if (!layout) continue;
            exprs.push_back((BinaryenExpressionRef)emit_i32_store(
                (uintptr_t)BinaryenConst(m_module, BinaryenLiteralInt32((int32_t)(base + layout->offset))),
                compile_node(f->data.assign.right)));
        }
    }
    exprs.push_back(BinaryenConst(m_module, BinaryenLiteralInt32((int32_t)base)));
    return (uintptr_t)BinaryenBlock(m_module, nullptr, exprs.data(), exprs.size(), BinaryenTypeInt32());
}

uintptr_t WasmCodegen::compile_member_assign(ASTNode *assign_node) {
    ASTNode *target = assign_node->data.assign.left;
    if (!target || target->type != AST_MEMBER_ACCESS) return (uintptr_t)BinaryenNop(m_module);

    uintptr_t val = compile_node(assign_node->data.assign.right);
    uintptr_t base = compile_node(target->data.member_access.object);
    ASTNode *field = target->data.member_access.field;

    if (!field || field->type != AST_IDENTIFIER || !field->data.identifier.name) {
        return (uintptr_t)BinaryenNop(m_module);
    }
    std::string fname(field->data.identifier.name);
    if (fname == "length") return (uintptr_t)BinaryenNop(m_module);

    ASTNode *object = target->data.member_access.object;
    uint32_t fieldOffset = 0;
    if (object && object->inferred_type && object->inferred_type->name) {
        std::string sname(object->inferred_type->name);
        const WasmFieldLayout *layout = find_field_layout(sname, fname);
        if (layout) fieldOffset = layout->offset;
    }

    uintptr_t addr = (uintptr_t)BinaryenBinary(
        m_module, BinaryenAddInt32(),
        (BinaryenExpressionRef)base,
        BinaryenConst(m_module, BinaryenLiteralInt32((int32_t)fieldOffset)));
    return emit_i32_store(addr, val);
}

uintptr_t WasmCodegen::compile_node(ASTNode *node) {
    if (!node || m_has_error) return (uintptr_t)BinaryenNop(m_module);

    switch (node->type) {
        case AST_PROGRAM:
            return compile_block(node);
        case AST_IF:
            return compile_if(node);
        case AST_WHILE:
            return compile_while(node);
        case AST_BINOP:
            return compile_binary_op(node);
        case AST_CALL:
            return compile_call(node);
        case AST_PRINT:
            return compile_print(node);
        case AST_EXPRESSION_LIST:
            if (node->inferred_type && node->inferred_type->kind == TYPEINFO_ARRAY) {
                return compile_array_literal(node);
            }
            return compile_block(node);
        case AST_STRUCT_LITERAL:
            return compile_struct_literal(node);
        case AST_MEMBER_ACCESS:
            return compile_member_access(node);
        case AST_INDEX:
            return compile_index(node);
        case AST_IDENTIFIER:
            return compile_ident(node);
        case AST_NUM_INT:
            return (uintptr_t)BinaryenConst(m_module, BinaryenLiteralInt32((int32_t)node->data.num_int.value));
        case AST_NUM_FLOAT:
            return (uintptr_t)BinaryenConst(m_module, BinaryenLiteralFloat64(node->data.num_float.value));
        case AST_STRING: {
            const char *s = node->data.string.value;
            if (!s) return (uintptr_t)BinaryenConst(m_module, BinaryenLiteralInt32(0));
            uint32_t len = strlen(s);
            StringEntry entry;
            entry.offset = m_string_offset;
            entry.data.assign(s, s + len + 1);
            m_strings.push_back(entry);
            m_string_offset += len + 1;
            m_string_offset = (m_string_offset + 3) & ~3;
            return (uintptr_t)BinaryenConst(m_module, BinaryenLiteralInt32((int32_t)entry.offset));
        }
        case AST_FOR:
            return compile_for(node);
        case AST_BREAK:
            return compile_break(node);
        case AST_CONTINUE:
            return compile_continue(node);
        case AST_RETURN:
            return compile_return(node);
        case AST_ASSIGN:
            if (node->data.assign.is_declaration) {
                return compile_var_decl(node, node->data.assign.right);
            }
            return compile_assign(node);
        default:
            return (uintptr_t)BinaryenNop(m_module);
    }
}

uintptr_t WasmCodegen::compile_block(ASTNode *node) {
    if (!node) return (uintptr_t)BinaryenNop(m_module);

    std::vector<uintptr_t> exprs;

    if (node->type == AST_PROGRAM) {
        for (int i = 0; i < node->data.program.statement_count; i++) {
            ASTNode *stmt = node->data.program.statements[i];
            if (stmt) {
                uintptr_t expr = compile_node(stmt);
                if (expr) exprs.push_back(expr);
            }
        }
    } else {
        uintptr_t expr = compile_node(node);
        if (expr) exprs.push_back(expr);
    }

    if (exprs.empty()) return (uintptr_t)BinaryenNop(m_module);
    if (exprs.size() == 1) return exprs[0];

    return (uintptr_t)BinaryenBlock(m_module, nullptr,
                                    (BinaryenExpressionRef*)exprs.data(),
                                    exprs.size(), BinaryenTypeAuto());
}

uintptr_t WasmCodegen::compile_if(ASTNode *if_node) {
    uintptr_t cond = compile_node(if_node->data.if_stmt.condition);
    uintptr_t then_expr = compile_node(if_node->data.if_stmt.then_body);
    uintptr_t else_expr = if_node->data.if_stmt.else_body
                          ? compile_node(if_node->data.if_stmt.else_body)
                          : (uintptr_t)BinaryenNop(m_module);
    return (uintptr_t)BinaryenIf(m_module, (BinaryenExpressionRef)cond,
                                 (BinaryenExpressionRef)then_expr,
                                 (BinaryenExpressionRef)else_expr);
}

uintptr_t WasmCodegen::compile_while(ASTNode *while_node) {
    std::string breakLabel = "while_break_" + std::to_string(m_break_labels.size());
    std::string continueLabel = "while_continue_" + std::to_string(m_continue_labels.size());
    m_break_labels.push_back(breakLabel);
    m_continue_labels.push_back(continueLabel);

    BinaryenExpressionRef cond = (BinaryenExpressionRef)compile_node(while_node->data.while_stmt.condition);
    BinaryenExpressionRef body = (BinaryenExpressionRef)compile_node(while_node->data.while_stmt.body);
    BinaryenExpressionRef notCond = BinaryenUnary(m_module, BinaryenEqZInt32(), cond);
    BinaryenExpressionRef breakIf = BinaryenBreak(m_module, breakLabel.c_str(), notCond, nullptr);

    BinaryenExpressionRef loopExprs[] = {
        breakIf,
        body,
        BinaryenBreak(m_module, continueLabel.c_str(), nullptr, nullptr)
    };

    BinaryenExpressionRef loopBody = BinaryenBlock(
        m_module, continueLabel.c_str(), loopExprs, 3, BinaryenTypeNone());
    BinaryenExpressionRef loop = BinaryenLoop(m_module, continueLabel.c_str(), loopBody);
    BinaryenExpressionRef outer[] = { loop };

    m_break_labels.pop_back();
    m_continue_labels.pop_back();
    return (uintptr_t)BinaryenBlock(m_module, breakLabel.c_str(), outer, 1, BinaryenTypeNone());
}

uintptr_t WasmCodegen::compile_for(ASTNode *for_node) {
    ASTNode *var = for_node->data.for_stmt.var;
    ASTNode *start = for_node->data.for_stmt.start;
    ASTNode *end = for_node->data.for_stmt.end;
    ASTNode *body = for_node->data.for_stmt.body;

    if (!var || var->type != AST_IDENTIFIER || !var->data.identifier.name) {
        return (uintptr_t)BinaryenNop(m_module);
    }
    const char *varName = var->data.identifier.name;

    ASTNode fakeDecl = {};
    fakeDecl.type = AST_ASSIGN;
    fakeDecl.data.assign.left = var;
    fakeDecl.data.assign.right = start;
    fakeDecl.data.assign.is_declaration = 1;

    uintptr_t init = compile_var_decl(&fakeDecl, start);
    uint32_t idx = get_or_create_local(varName, BinaryenTypeInt32());

    uintptr_t condVal = compile_node(end);
    BinaryenExpressionRef cond = BinaryenBinary(
        m_module,
        BinaryenLtSInt32(),
        BinaryenLocalGet(m_module, idx, BinaryenTypeInt32()),
        (BinaryenExpressionRef)condVal);

    BinaryenExpressionRef inc = BinaryenLocalSet(
        m_module,
        idx,
        BinaryenBinary(
            m_module,
            BinaryenAddInt32(),
            BinaryenLocalGet(m_module, idx, BinaryenTypeInt32()),
            BinaryenConst(m_module, BinaryenLiteralInt32(1))));

    std::string breakLabel = "for_break_" + std::to_string(m_break_labels.size());
    std::string continueLabel = "for_continue_" + std::to_string(m_continue_labels.size());
    m_break_labels.push_back(breakLabel);
    m_continue_labels.push_back(continueLabel);

    BinaryenExpressionRef notCond = BinaryenUnary(m_module, BinaryenEqZInt32(), cond);
    BinaryenExpressionRef breakIf = BinaryenBreak(m_module, breakLabel.c_str(), notCond, nullptr);

    BinaryenExpressionRef bodyExpr = (BinaryenExpressionRef)compile_node(body);
    BinaryenExpressionRef loopExprs[] = {
        breakIf,
        bodyExpr,
        inc,
        BinaryenBreak(m_module, continueLabel.c_str(), nullptr, nullptr)
    };

    BinaryenExpressionRef loopBody = BinaryenBlock(
        m_module, continueLabel.c_str(), loopExprs, 4, BinaryenTypeNone());
    BinaryenExpressionRef loop = BinaryenLoop(m_module, continueLabel.c_str(), loopBody);
    BinaryenExpressionRef outer[] = { loop };

    m_break_labels.pop_back();
    m_continue_labels.pop_back();

    BinaryenExpressionRef blockExprs[] = {
        (BinaryenExpressionRef)init,
        BinaryenBlock(m_module, breakLabel.c_str(), outer, 1, BinaryenTypeNone())
    };
    return (uintptr_t)BinaryenBlock(m_module, nullptr, blockExprs, 2, BinaryenTypeNone());
}

uintptr_t WasmCodegen::compile_break(ASTNode *) {
    if (m_break_labels.empty()) return (uintptr_t)BinaryenNop(m_module);
    return (uintptr_t)BinaryenBreak(m_module, m_break_labels.back().c_str(), nullptr, nullptr);
}

uintptr_t WasmCodegen::compile_continue(ASTNode *) {
    if (m_continue_labels.empty()) return (uintptr_t)BinaryenNop(m_module);
    return (uintptr_t)BinaryenBreak(m_module, m_continue_labels.back().c_str(), nullptr, nullptr);
}

uintptr_t WasmCodegen::compile_binary_op(ASTNode *op_node) {
    uintptr_t left = compile_node(op_node->data.binop.left);
    uintptr_t right = compile_node(op_node->data.binop.right);

    BinaryenOp op;
    switch (op_node->data.binop.op) {
        case OP_ADD: op = BinaryenAddInt32(); break;
        case OP_SUB: op = BinaryenSubInt32(); break;
        case OP_MUL: op = BinaryenMulInt32(); break;
        case OP_DIV: op = BinaryenDivSInt32(); break;
        case OP_MOD: op = BinaryenRemSInt32(); break;
        case OP_EQ:  op = BinaryenEqInt32(); break;
        case OP_NE:  op = BinaryenNeInt32(); break;
        case OP_LT:  op = BinaryenLtSInt32(); break;
        case OP_LE:  op = BinaryenLeSInt32(); break;
        case OP_GT:  op = BinaryenGtSInt32(); break;
        case OP_GE:  op = BinaryenGeSInt32(); break;
        case OP_AND: op = BinaryenAndInt32(); break;
        case OP_OR:  op = BinaryenOrInt32(); break;
        default:
            set_error("unsupported binary operator");
            return (uintptr_t)BinaryenUnreachable(m_module);
    }
    return (uintptr_t)BinaryenBinary(m_module, op,
                                     (BinaryenExpressionRef)left,
                                     (BinaryenExpressionRef)right);
}

uintptr_t WasmCodegen::compile_call(ASTNode *call_node) {
    ASTNode *func = call_node->data.call.func;
    ASTNode *args = call_node->data.call.args;

    if (!func || func->type != AST_IDENTIFIER || !func->data.identifier.name) {
        set_error("call target is not an identifier");
        return (uintptr_t)BinaryenUnreachable(m_module);
    }

    std::string name(func->data.identifier.name);
    std::string wasm_name = name;

    if (name == "print" || name == "puts") {
        wasm_name = "vix_puts";
    } else if (name == "putchar") {
        wasm_name = "vix_putchar";
    } else if (name == "exit") {
        wasm_name = "vix_exit";
    }

    bool is_import = (name == "putchar" || name == "puts" || name == "print" || name == "exit");

    std::vector<uintptr_t> arg_exprs;
    if (args && args->type == AST_EXPRESSION_LIST) {
        for (int i = 0; i < args->data.expression_list.expression_count; i++) {
            uintptr_t a = compile_node(args->data.expression_list.expressions[i]);
            arg_exprs.push_back(a);
        }
    }

    uintptr_t ret_type = BinaryenTypeInt32();
    if (is_import) {
        ret_type = BinaryenTypeNone();
    }

    return (uintptr_t)BinaryenCall(m_module, wasm_name.c_str(),
                                   (BinaryenExpressionRef*)arg_exprs.data(),
                                   arg_exprs.size(), ret_type);
}

uintptr_t WasmCodegen::compile_print(ASTNode *print_node) {
    ASTNode *expr = print_node ? print_node->data.print.expr : nullptr;
    if (!expr) return (uintptr_t)BinaryenNop(m_module);

    std::vector<uintptr_t> exprs;
    auto append_print = [&](ASTNode *item) {
        if (!item) return;

        uintptr_t value = compile_node(item);
        const char *import_name = "vix_print_i32";

        if (item->type == AST_STRING ||
            (item->inferred_type && item->inferred_type->kind == TYPEINFO_STRING)) {
            import_name = "vix_puts";
        } else if (item->type == AST_CHAR) {
            import_name = "vix_putchar";
        }

        BinaryenExpressionRef arg = (BinaryenExpressionRef)value;
        exprs.push_back((uintptr_t)BinaryenCall(
            m_module,
            import_name,
            &arg,
            1,
            BinaryenTypeNone()));
    };

    if (expr->type == AST_EXPRESSION_LIST) {
        for (int i = 0; i < expr->data.expression_list.expression_count; i++) {
            append_print(expr->data.expression_list.expressions[i]);
        }
    } else {
        append_print(expr);
    }

    if (exprs.empty()) return (uintptr_t)BinaryenNop(m_module);
    if (exprs.size() == 1) return exprs[0];

    return (uintptr_t)BinaryenBlock(m_module, nullptr,
                                    (BinaryenExpressionRef*)exprs.data(),
                                    exprs.size(), BinaryenTypeNone());
}

uintptr_t WasmCodegen::compile_ident(ASTNode *ident_node) {
    if (!ident_node->data.identifier.name) {
        return (uintptr_t)BinaryenNop(m_module);
    }
    uint32_t idx = get_or_create_local(ident_node->data.identifier.name, BinaryenTypeInt32());
    return (uintptr_t)BinaryenLocalGet(m_module, idx, BinaryenTypeInt32());
}

uintptr_t WasmCodegen::compile_return(ASTNode *ret_node) {
    if (!ret_node->data.return_stmt.expr) {
        return (uintptr_t)BinaryenReturn(m_module, nullptr);
    }
    uintptr_t val = compile_node(ret_node->data.return_stmt.expr);
    return (uintptr_t)BinaryenReturn(m_module, (BinaryenExpressionRef)val);
}

uintptr_t WasmCodegen::compile_member_access(ASTNode *node) {
    ASTNode *object = node->data.member_access.object;
    ASTNode *field = node->data.member_access.field;
    if (!object || !field || field->type != AST_IDENTIFIER || !field->data.identifier.name) {
        return (uintptr_t)BinaryenNop(m_module);
    }
    std::string fname(field->data.identifier.name);
    uintptr_t base = compile_node(object);
    if (fname == "length") {
        return emit_array_length(base);
    }
    uint32_t fieldOffset = 0;
    if (object && object->inferred_type && object->inferred_type->name) {
        std::string sname(object->inferred_type->name);
        const WasmFieldLayout *layout = find_field_layout(sname, fname);
        if (layout) fieldOffset = layout->offset;
    }
    uintptr_t addr = (uintptr_t)BinaryenBinary(
        m_module, BinaryenAddInt32(),
        (BinaryenExpressionRef)base,
        BinaryenConst(m_module, BinaryenLiteralInt32((int32_t)fieldOffset)));
    return emit_i32_load(addr);
}

uintptr_t WasmCodegen::compile_assign(ASTNode *assign_node) {
    ASTNode *target = assign_node->data.assign.left;
    if (target && target->type == AST_INDEX) {
        return compile_index_assign(assign_node);
    }
    if (target && target->type == AST_MEMBER_ACCESS) {
        return compile_member_assign(assign_node);
    }

    uintptr_t val = compile_node(assign_node->data.assign.right);
    if (target && target->type == AST_IDENTIFIER && target->data.identifier.name) {
        uint32_t idx = get_or_create_local(target->data.identifier.name, BinaryenTypeInt32());
        return (uintptr_t)BinaryenLocalSet(m_module, idx, (BinaryenExpressionRef)val);
    }

    return (uintptr_t)BinaryenNop(m_module);
}

uintptr_t WasmCodegen::compile_var_decl(ASTNode *decl_node, ASTNode *init_expr) {
    if (decl_node->type != AST_ASSIGN) return (uintptr_t)BinaryenNop(m_module);
    ASTNode *target = decl_node->data.assign.left;
    if (target && target->type == AST_IDENTIFIER && target->data.identifier.name) {
        uintptr_t val;
        if (init_expr) {
            val = compile_node(init_expr);
        } else {
            val = (uintptr_t)BinaryenConst(m_module, BinaryenLiteralInt32(0));
        }
        return (uintptr_t)BinaryenLocalSet(m_module,
            get_or_create_local(target->data.identifier.name, BinaryenTypeInt32()),
            (BinaryenExpressionRef)val);
    }
    return (uintptr_t)BinaryenNop(m_module);
}

bool WasmCodegen::emit(ASTNode *root, std::vector<uint8_t> &out_bytes, std::string &error_msg) {
    if (!root) {
        error_msg = "null AST root";
        return false;
    }

    m_module = BinaryenModuleCreate();
    m_has_error = false;
    m_error.clear();
    m_functions.clear();
    m_strings.clear();
    m_current_func = nullptr;
    m_string_offset = 16;
    m_heap_offset = 4096;
    m_break_labels.clear();
    m_continue_labels.clear();

    add_imports();

    if (root->type == AST_PROGRAM) {
        for (int i = 0; i < root->data.program.statement_count; i++) {
            ASTNode *stmt = root->data.program.statements[i];
            if (stmt && stmt->type == AST_STRUCT_DEF) {
                register_struct_layout(stmt);
            }
            if (stmt && stmt->type == AST_FUNCTION) {
                register_function(stmt);
            }
        }
    }

    if (root->type == AST_PROGRAM) {
        for (int i = 0; i < root->data.program.statement_count; i++) {
            ASTNode *stmt = root->data.program.statements[i];
            if (stmt && stmt->type == AST_FUNCTION) {
                compile_function_body(stmt);
            }
        }
    }

    size_t num_seg = m_strings.size();
    if (num_seg == 0) {
        BinaryenSetMemory(m_module, 1, -1, "memory", nullptr, nullptr, nullptr, nullptr, nullptr, 0, false, false, nullptr);
    } else {
        std::vector<const char*> seg_names(num_seg, nullptr);
        std::vector<const char*> seg_datas(num_seg);
        std::vector<uint8_t> seg_passives(num_seg, 0);
        std::vector<BinaryenExpressionRef> seg_offsets(num_seg);
        std::vector<BinaryenIndex> seg_sizes(num_seg);
        for (size_t i = 0; i < num_seg; i++) {
            seg_datas[i] = m_strings[i].data.data();
            seg_sizes[i] = m_strings[i].data.size();
            seg_offsets[i] = BinaryenConst(m_module, BinaryenLiteralInt32((int32_t)m_strings[i].offset));
        }
        if (m_heap_offset < m_string_offset + 16) {
            m_heap_offset = (m_string_offset + 31) & ~31;
        }
        uint32_t mem_pages = (m_heap_offset + 65535) / 65536;
        if (mem_pages < 1) mem_pages = 1;
        BinaryenSetMemory(m_module, mem_pages, -1, "memory",
                           seg_names.data(), seg_datas.data(),
                           (bool*)seg_passives.data(), seg_offsets.data(),
                           seg_sizes.data(), (BinaryenIndex)num_seg,
                           false, false, nullptr);
    }

    if (m_functions.count("main")) {
        BinaryenAddFunctionExport(m_module, "main", "main");
    }

    if (m_has_error) {
        BinaryenModuleDispose(m_module);
        m_module = nullptr;
        error_msg = m_error;
        return false;
    }

    BinaryenModuleAllocateAndWriteResult write_result = BinaryenModuleAllocateAndWrite(m_module, nullptr);
    if (write_result.binary) {
        out_bytes.assign((uint8_t*)write_result.binary,
                         (uint8_t*)write_result.binary + write_result.binaryBytes);
        free(write_result.binary);
        return true;
    }

    error_msg = "Failed to write WASM binary";
    return false;
}
