#include "../../include/ownership.h"
#include "../../include/compiler.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <cctype>

extern "C" const char* current_input_filename;

namespace {

enum class ExprUse { Read, Move, Address, AssignTarget };

struct VarState {
    const TypeInfo* type = nullptr;
    bool is_mutable = false;
    bool moved = false;
    bool is_global = false;
    bool borrowed_shared = false;
    bool borrowed_mut = false;
    std::string borrow_source;
};

struct Scope {
    std::unordered_map<std::string, VarState> vars;
};

struct ExprInfo {
    const TypeInfo* type = nullptr;
    bool copy = true;
    bool is_ref = false;
    std::string borrow_source;
};

class OwnershipChecker {
public:
    int check(ASTNode* root) {
        push_scope();
        check_node(root);
        pop_scope();
        return errors;
    }

private:
    std::vector<Scope> scopes;
    int errors = 0;

    static const char* node_file(const ASTNode* node) {
        if (node && node->source_file) return node->source_file;
        return current_input_filename ? current_input_filename : "unknown";
    }

    static int node_line(const ASTNode* node) {
        return node && node->location.first_line > 0 ? node->location.first_line : 1;
    }

    static int node_col(const ASTNode* node) {
        return node && node->location.first_column > 0 ? node->location.first_column : 1;
    }

    static bool is_copy_type(const TypeInfo* t) {
        if (!t) return true;
        switch (t->kind) {
            case TYPEINFO_VOID:
            case TYPEINFO_I8:
            case TYPEINFO_I32:
            case TYPEINFO_I64:
            case TYPEINFO_F32:
            case TYPEINFO_F64:
            case TYPEINFO_BOOL:
            case TYPEINFO_PTR:
            case TYPEINFO_FN:
            case TYPEINFO_VAR:
                return true;
            case TYPEINFO_STRING:
                return false;
            case TYPEINFO_STRUCT:
                if (t->params && t->param_count > 0) {
                    for (int i = 0; i < t->param_count; i++) {
                        if (!is_copy_type(t->params[i])) return false;
                    }
                }
                return true;
            case TYPEINFO_FIXED_ARRAY:
                return t->size <= 16 && is_copy_type(t->element);
            case TYPEINFO_ARRAY:
            case TYPEINFO_APP:
                return false;
        }
        return true;
    }

    void push_scope() { scopes.emplace_back(); }

    void pop_scope() {
        if (!scopes.empty()) scopes.pop_back();
    }

    void report(ASTNode* node, const std::string& message) {
        set_location_with_column(node_file(node), node_line(node), node_col(node));
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_SEMANTIC, message.c_str());
        errors++;
    }

    VarState* lookup(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->vars.find(name);
            if (found != it->vars.end()) return &found->second;
        }
        return nullptr;
    }

    bool is_local_name(const std::string& name) const {
        if (name.empty()) return false;
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->vars.find(name);
            if (found != it->vars.end()) return !found->second.is_global;
        }
        return false;
    }

    void declare_var(const std::string& name, const TypeInfo* type, bool is_mutable,
                     bool is_global, const std::string& borrow_source = {}) {
        if (scopes.empty()) push_scope();
        VarState state;
        state.type = type;
        state.is_mutable = is_mutable;
        state.is_global = is_global;
        state.borrow_source = borrow_source;
        scopes.back().vars[name] = std::move(state);
    }

    void end_statement() {
        // Collect all active borrow sources from variables still in scope
        std::unordered_set<std::string> active_borrow_sources;
        for (auto& scope : scopes) {
            for (auto& entry : scope.vars) {
                if (!entry.second.borrow_source.empty()) {
                    active_borrow_sources.insert(entry.second.borrow_source);
                }
            }
        }
        // Only clear borrow flags for variables NOT currently borrowed by live variables
        for (auto& scope : scopes) {
            for (auto& entry : scope.vars) {
                if (active_borrow_sources.find(entry.first) == active_borrow_sources.end()) {
                    entry.second.borrowed_shared = false;
                    entry.second.borrowed_mut = false;
                }
            }
        }
    }

    std::string lvalue_base(ASTNode* node) {
        if (!node) return {};
        switch (node->type) {
            case AST_IDENTIFIER:
                return node->data.identifier.name ? node->data.identifier.name : "";
            case AST_MEMBER_ACCESS:
                return lvalue_base(node->data.member_access.object);
            case AST_INDEX:
                return lvalue_base(node->data.index.target);
            case AST_UNARYOP:
                if (node->data.unaryop.op == OP_DEREF) return lvalue_base(node->data.unaryop.expr);
                return {};
            default:
                return {};
        }
    }

    void check_node(ASTNode* node) {
        if (!node) return;
        switch (node->type) {
            case AST_PROGRAM:
                check_block(node, false);
                break;
            case AST_FUNCTION:
                check_function(node);
                break;
            case AST_STRUCT_DEF:
            case AST_IMPORT:
                break;
            case AST_GLOBAL:
                check_global(node);
                end_statement();
                break;
            default:
                check_stmt(node);
                break;
        }
    }

    void check_block(ASTNode* block, bool new_scope) {
        if (!block) return;
        if (new_scope) push_scope();
        if (block->type == AST_PROGRAM) {
            for (int i = 0; i < block->data.program.statement_count; i++) {
                check_stmt(block->data.program.statements[i]);
            }
        } else {
            check_stmt(block);
        }
        if (new_scope) pop_scope();
    }

    void check_function(ASTNode* fn) {
        if (!fn || fn->data.function.is_extern) return;
        push_scope();
        ASTNode* params = fn->data.function.params;
        if (params && params->type == AST_EXPRESSION_LIST) {
            for (int i = 0; i < params->data.expression_list.expression_count; i++) {
                ASTNode* param = params->data.expression_list.expressions[i];
                if (!param) continue;
                ASTNode* id = param;
                const TypeInfo* type = param->inferred_type;
                bool mut = param->mutability == MUTABILITY_MUTABLE;
                if (param->type == AST_ASSIGN) {
                    id = param->data.assign.left;
                    type = param->inferred_type ? param->inferred_type :
                           (param->data.assign.right ? param->data.assign.right->inferred_type : nullptr);
                    mut = param->data.assign.mutability == MUTABILITY_MUTABLE ||
                          (id && id->mutability == MUTABILITY_MUTABLE);
                }
                if (id && id->type == AST_IDENTIFIER && id->data.identifier.name) {
                    declare_var(id->data.identifier.name, type, mut, false);
                }
            }
        }
        check_block(fn->data.function.body, false);
        pop_scope();
    }

    void check_global(ASTNode* node) {
        if (!node || !node->data.global_decl.identifier ||
            node->data.global_decl.identifier->type != AST_IDENTIFIER) {
            return;
        }
        ExprInfo init = check_expr(node->data.global_decl.initializer, ExprUse::Read);
        const char* name = node->data.global_decl.identifier->data.identifier.name;
        if (name) declare_var(name, node->inferred_type ? node->inferred_type : init.type,
                              true, true, init.borrow_source);
    }

    void check_stmt(ASTNode* stmt) {
        if (!stmt) return;
        switch (stmt->type) {
            case AST_PROGRAM:
                check_block(stmt, true);
                return;
            case AST_FUNCTION:
                check_function(stmt);
                return;
            case AST_ASSIGN:
            case AST_CONST:
                check_assign(stmt);
                end_statement();
                return;
            case AST_PRINT:
                check_expr(stmt->data.print.expr, ExprUse::Read);
                end_statement();
                return;
            case AST_RETURN:
                check_return(stmt);
                end_statement();
                return;
            case AST_IF:
                check_expr(stmt->data.if_stmt.condition, ExprUse::Read);
                end_statement();
                {
                    std::unordered_map<std::string, bool> saved_moved;
                    for (auto& scope : scopes) {
                        for (auto& entry : scope.vars) {
                            saved_moved[entry.first] = entry.second.moved;
                        }
                    }
                    check_block(stmt->data.if_stmt.then_body, true);
                    if (stmt->data.if_stmt.else_body) check_block(stmt->data.if_stmt.else_body, true);
                    for (auto& scope : scopes) {
                        for (auto& entry : scope.vars) {
                            auto it = saved_moved.find(entry.first);
                            if (it != saved_moved.end()) {
                                entry.second.moved = it->second;
                            }
                        }
                    }
                }
                return;
            case AST_WHILE:
                check_expr(stmt->data.while_stmt.condition, ExprUse::Read);
                end_statement();
                check_block(stmt->data.while_stmt.body, true);
                return;
            case AST_FOR:
                check_expr(stmt->data.for_stmt.start, ExprUse::Read);
                check_expr(stmt->data.for_stmt.end, ExprUse::Read);
                end_statement();
                {
                    // Save moved state of all variables before loop body
                    // (loop might not execute, so we need to restore after)
                    std::unordered_map<std::string, bool> saved_moved;
                    for (auto& scope : scopes) {
                        for (auto& entry : scope.vars) {
                            saved_moved[entry.first] = entry.second.moved;
                        }
                    }
                    push_scope();
                    if (stmt->data.for_stmt.var && stmt->data.for_stmt.var->type == AST_IDENTIFIER &&
                        stmt->data.for_stmt.var->data.identifier.name) {
                        declare_var(stmt->data.for_stmt.var->data.identifier.name,
                                    stmt->data.for_stmt.var->inferred_type, false, false);
                    }
                    check_block(stmt->data.for_stmt.body, false);
                    pop_scope();
                    // Restore moved state (loop body might not have executed)
                    for (auto& scope : scopes) {
                        for (auto& entry : scope.vars) {
                            auto it = saved_moved.find(entry.first);
                            if (it != saved_moved.end()) {
                                entry.second.moved = it->second;
                            }
                        }
                    }
                }
                return;
            case AST_BREAK:
            case AST_CONTINUE:
                end_statement();
                return;
            default:
                check_expr(stmt, ExprUse::Read);
                end_statement();
                return;
        }
    }

    void check_assign(ASTNode* node) {
        ASTNode* left = node->data.assign.left;
        ASTNode* right = node->data.assign.right;
        bool is_decl = node->data.assign.is_declaration != 0;
        ExprInfo rhs = check_expr(right, ExprUse::Move);

        if (is_decl && left && left->type == AST_IDENTIFIER && left->data.identifier.name) {
            bool mut = left->mutability == MUTABILITY_MUTABLE || node->data.assign.mutability == MUTABILITY_MUTABLE;
            const TypeInfo* type = right ? right->inferred_type : nullptr;
            if (!type || type->kind == TYPEINFO_VOID) type = rhs.type;
            declare_var(left->data.identifier.name, type ? type : rhs.type, mut,
                        scopes.size() == 1, rhs.borrow_source);
            return;
        }

        std::string base = lvalue_base(left);
        if (!base.empty()) {
            VarState* target = lookup(base);
            if (target) {
                if (target->moved) report(left, "use of moved value '" + base + "'");
                if (target->borrowed_shared || target->borrowed_mut) {
                    report(left, "cannot assign to '" + base + "' while it is borrowed");
                }
                target->moved = false;
                target->borrow_source = rhs.borrow_source;
            }
        }
        check_expr(left, ExprUse::AssignTarget);
    }

    void check_return(ASTNode* node) {
        ExprInfo value = check_expr(node->data.return_stmt.expr, ExprUse::Move);
        if (!value.borrow_source.empty() && is_local_name(value.borrow_source)) {
            report(node->data.return_stmt.expr ? node->data.return_stmt.expr : node,
                   "cannot return reference to local variable '" + value.borrow_source + "'");
        }
    }

    ExprInfo check_expr(ASTNode* node, ExprUse use) {
        ExprInfo info;
        if (!node) return info;
        info.type = node->inferred_type;
        info.copy = is_copy_type(node->inferred_type);

        switch (node->type) {
            case AST_IDENTIFIER:
                return check_identifier(node, use);
            case AST_NUM_INT:
            case AST_NUM_FLOAT:
            case AST_CHAR:
            case AST_NIL:
                info.copy = true;
                return info;
            case AST_STRING:
                info.copy = false;
                return info;
            case AST_UNARYOP:
                return check_unary(node);
            case AST_BINOP:
                check_expr(node->data.binop.left, ExprUse::Read);
                check_expr(node->data.binop.right, ExprUse::Read);
                return info;
            case AST_CALL:
                return check_call(node);
            case AST_EXPRESSION_LIST:
                return check_expression_list(node, use);
            case AST_INDEX:
                check_expr(node->data.index.target, ExprUse::Read);
                check_expr(node->data.index.index, ExprUse::Read);
                return info;
            case AST_MEMBER_ACCESS:
                check_expr(node->data.member_access.object, ExprUse::Read);
                return info;
            case AST_STRUCT_LITERAL:
                return check_struct_literal(node);
            case AST_IF:
                check_stmt(node);
                return info;
            case AST_FUNCTION:
                check_function(node);
                info.copy = true;
                return info;
            case AST_TOINT:
                check_expr(node->data.toint.expr, ExprUse::Read);
                return info;
            case AST_TOFLOAT:
                check_expr(node->data.tofloat.expr, ExprUse::Read);
                return info;
            case AST_INPUT:
                check_expr(node->data.input.prompt, ExprUse::Read);
                return info;
            default:
                return info;
        }
    }

    ExprInfo check_identifier(ASTNode* node, ExprUse use) {
        ExprInfo info;
        info.type = node->inferred_type;
        info.copy = is_copy_type(node->inferred_type);
        const char* cname = node->data.identifier.name;
        if (!cname) return info;
        VarState* state = lookup(cname);
        if (!state) return info;
        info.type = state->type ? state->type : node->inferred_type;
        info.copy = is_copy_type(info.type);
        if (std::isupper(static_cast<unsigned char>(cname[0]))) info.copy = true;
        info.borrow_source = state->borrow_source;

        if (use != ExprUse::AssignTarget && state->moved) {
            report(node, std::string("use of moved value '") + cname + "'");
            return info;
        }
        if (use == ExprUse::Move && !info.copy) {
            if (state->borrowed_shared || state->borrowed_mut) {
                report(node, std::string("cannot move '") + cname + "' while it is borrowed");
            } else {
                state->moved = true;
            }
        }
        return info;
    }

    ExprInfo check_unary(ASTNode* node) {
        ExprInfo info;
        info.type = node->inferred_type;
        info.copy = is_copy_type(node->inferred_type);
        UnaryOpType op = node->data.unaryop.op;
        if (op == OP_ADDRESS) {
            std::string base = lvalue_base(node->data.unaryop.expr);
            if (!base.empty()) {
                VarState* state = lookup(base);
                if (state) {
                    if (state->moved) report(node, "cannot borrow moved value '" + base + "'");
                    bool wants_mut = node->mutability == MUTABILITY_MUTABLE;
                    if (wants_mut) {
                        if (state->borrowed_shared || state->borrowed_mut) {
                            report(node, "cannot mutably borrow '" + base + "' more than once");
                        }
                        state->borrowed_mut = true;
                    } else {
                        if (state->borrowed_mut) {
                            report(node, "cannot immutably borrow '" + base + "' while it is mutably borrowed");
                        }
                        state->borrowed_shared = true;
                    }
                    info.borrow_source = base;
                }
            }
            check_expr(node->data.unaryop.expr, ExprUse::Address);
            info.is_ref = true;
            info.copy = true;
            return info;
        }
        if (op == OP_DEREF) {
            ExprInfo inner = check_expr(node->data.unaryop.expr, ExprUse::Read);
            info.borrow_source = inner.borrow_source;
            return info;
        }
        check_expr(node->data.unaryop.expr, ExprUse::Read);
        return info;
    }

    ExprInfo check_call(ASTNode* node) {
        ExprInfo info;
        info.type = node->inferred_type;
        info.copy = is_copy_type(node->inferred_type);
        check_expr(node->data.call.func, ExprUse::Read);
        ASTNode* args = node->data.call.args;
        bool returns_ref = node->inferred_type && node->inferred_type->kind == TYPEINFO_PTR;
        if (args && args->type == AST_EXPRESSION_LIST) {
            for (int i = 0; i < args->data.expression_list.expression_count; i++) {
                ExprInfo arg_info = check_expr(args->data.expression_list.expressions[i], ExprUse::Read);
                // Propagate borrow_source from reference arguments only when function returns a pointer
                if (returns_ref && info.borrow_source.empty() && !arg_info.borrow_source.empty()) {
                    info.borrow_source = arg_info.borrow_source;
                }
            }
        }
        return info;
    }

    ExprInfo check_expression_list(ASTNode* node, ExprUse use = ExprUse::Move) {
        ExprInfo info;
        info.type = node->inferred_type;
        info.copy = is_copy_type(node->inferred_type);
        for (int i = 0; i < node->data.expression_list.expression_count; i++) {
            check_expr(node->data.expression_list.expressions[i], use);
        }
        return info;
    }

    ExprInfo check_struct_literal(ASTNode* node) {
        ExprInfo info;
        info.type = node->inferred_type;
        info.copy = false;
        ASTNode* fields = node->data.struct_literal.fields;
        if (fields && fields->type == AST_EXPRESSION_LIST) {
            for (int i = 0; i < fields->data.expression_list.expression_count; i++) {
                ASTNode* field = fields->data.expression_list.expressions[i];
                ExprInfo elem;
                if (field && field->type == AST_ASSIGN) {
                    elem = check_expr(field->data.assign.right, ExprUse::Move);
                } else {
                    elem = check_expr(field, ExprUse::Move);
                }
                if (info.borrow_source.empty()) info.borrow_source = elem.borrow_source;
            }
        }
        return info;
    }
};

} // namespace

extern "C" int ownership_check_program(ASTNode* root) {
    OwnershipChecker checker;
    return checker.check(root) == 0 ? 0 : 1;
}
