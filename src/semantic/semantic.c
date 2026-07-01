/*
 * Copyright (c) 2026 Vix Language Authors. All rights reserved.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "../../include/semantic.h"
#include "../../include/compiler.h"
#include "../../include/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/compat.h"
#include <ctype.h>
extern const char* current_input_filename;
typedef struct VixVisitedModule VixVisitedModule;
struct VixVisitedModule {
    const char* path;
    VixVisitedModule* next;
};
static int extract_public_functions_from_module_recurse(const char* module_path, SymbolTable* table, VixVisitedModule* visited);
static int extract_public_functions_from_module(const char* module_path, SymbolTable* table);

static int is_builtin_union_ctor_name(const char* name) {
    if (!name) return 0;
    return strcmp(name, "Some") == 0 || strcmp(name, "None") == 0 ||
           strcmp(name, "Ok") == 0 || strcmp(name, "Err") == 0;
}

static const char* node_source_filename(const ASTNode* node) {
    if (node && node->source_file) {
        return node->source_file;
    }
    return current_input_filename ? current_input_filename : "unknown";
}

typedef struct VisitedNode {
    ASTNode* node;
    struct VisitedNode* next;
} VisitedNode;
static void clear_var_init_map(void);
static void add_var_init_mapping(const char* var_name, ASTNode* init_value);
static ASTNode* find_var_init_mapping(const char* var_name);
typedef struct VarInitMap {
    char* var_name;
    ASTNode* init_value;
    struct VarInitMap* next;
} VarInitMap;

static VarInitMap* g_var_init_map = NULL;
static void clear_var_init_map() {
    VarInitMap* cur = g_var_init_map;
    while (cur) {
        VarInitMap* next = cur->next;
        free(cur->var_name);
        free(cur);
        cur = next;
    }
    g_var_init_map = NULL;
}
static void add_var_init_mapping(const char* var_name, ASTNode* init_value) {
    if (!var_name || !init_value) return;
    VarInitMap* m = malloc(sizeof(VarInitMap));
    if (!m) return;
    m->var_name = malloc(strlen(var_name) + 1);
    if (!m->var_name) {
        free(m);
        return;
    }
    strcpy(m->var_name, var_name);
    m->init_value = init_value;
    m->next = g_var_init_map;
    g_var_init_map = m;
}

static ASTNode* find_var_init_mapping(const char* var_name) {
    VarInitMap* cur = g_var_init_map;
    while (cur) {
        if (strcmp(cur->var_name, var_name) == 0) return cur->init_value;
        cur = cur->next;
    }
    return NULL;
}

static int is_node_struct_field_assignment(ASTNode* node, VisitedNode* visited_list);
static int is_node_struct_field_assignment(ASTNode* node, VisitedNode* visited_list) {
    (void)node;
    
    VisitedNode* current = visited_list;
    while (current) {
        if (current->node && current->node->type == AST_STRUCT_DEF) {
            return 1;
        }
        current = current->next;
    }
    return 0;
}

static int is_node_inside_struct_literal(ASTNode* node, VisitedNode* visited_list);
static int is_node_inside_struct_literal(ASTNode* node, VisitedNode* visited_list) {
    (void)node;

    VisitedNode* current = visited_list;
    while (current) {
        if (current->node && current->node->type == AST_STRUCT_LITERAL) {
            return 1;
        }
        current = current->next;
    }
    return 0;
}

typedef struct StructDef {
    char* name;
    ASTNode* fields;
    struct StructDef* next;
} StructDef;
static StructDef* g_struct_definitions = NULL;
typedef struct VarStructMap {
    char* var_name;
    char* struct_name;
    struct VarStructMap* next;
} VarStructMap;

static VarStructMap* g_var_struct_map = NULL;

static void clear_var_struct_map() {
    VarStructMap* cur = g_var_struct_map;
    while (cur) {
        VarStructMap* next = cur->next;
        free(cur->var_name);
        free(cur->struct_name);
        free(cur);
        cur = next;
    }
    g_var_struct_map = NULL;
}

static void add_var_struct_mapping(const char* var_name, const char* struct_name) {
    if (!var_name || !struct_name) return;
    VarStructMap* m = malloc(sizeof(VarStructMap));
    if (!m) return;
    m->var_name = malloc(strlen(var_name) + 1);
    m->struct_name = malloc(strlen(struct_name) + 1);
    if (!m->var_name || !m->struct_name) {
        free(m->var_name);
        free(m->struct_name);
        free(m);
        return;
    }
    strcpy(m->var_name, var_name);
    strcpy(m->struct_name, struct_name);
    m->next = g_var_struct_map;
    g_var_struct_map = m;
}

static const char* find_var_struct_mapping(const char* var_name) {
    VarStructMap* cur = g_var_struct_map;
    while (cur) {
        if (strcmp(cur->var_name, var_name) == 0) return cur->struct_name;
        cur = cur->next;
    }
    return NULL;
}

static int levenshtein_distance(const char* s, const char* t) {
    if (!s || !t) return 1000000;
    int ls = strlen(s);
    int lt = strlen(t);
    int *v0 = malloc((lt + 1) * sizeof(int));
    int *v1 = malloc((lt + 1) * sizeof(int));
    if (!v0 || !v1) {
        free(v0); free(v1);
        return 1000000;
    }
    for (int i = 0; i <= lt; i++) v0[i] = i;
    for (int i = 0; i < ls; i++) {
        v1[0] = i + 1;
        for (int j = 0; j < lt; j++) {
            int cost = (s[i] == t[j]) ? 0 : 1;
            int deletion = v0[j + 1] + 1;
            int insertion = v1[j] + 1;
            int substitution = v0[j] + cost;
            int mv = deletion < insertion ? deletion : insertion;
            v1[j + 1] = mv < substitution ? mv : substitution;
        }
        int *tmp = v0; v0 = v1; v1 = tmp;
    }
    int res = v0[lt];
    free(v0); free(v1);
    return res;
}

static ASTNode* find_field_in_struct(StructDef* def, const char* field_name) {
    if (!def || !def->fields || def->fields->type != AST_EXPRESSION_LIST) return NULL;
    int fc = def->fields->data.expression_list.expression_count;
    for (int i = 0; i < fc; i++) {
        ASTNode* f = def->fields->data.expression_list.expressions[i];
        if (f && f->type == AST_ASSIGN && f->data.assign.left && f->data.assign.left->type == AST_IDENTIFIER) {
            if (strcmp(f->data.assign.left->data.identifier.name, field_name) == 0) return f;
        }
    }
    return NULL;
}

static const char* find_closest_field_name(StructDef* def, const char* field_name) {
    if (!def || !def->fields || !field_name) return NULL;
    int fc = def->fields->data.expression_list.expression_count;
    const char* best = NULL;
    int bestd = 1000000;
    for (int i = 0; i < fc; i++) {
        ASTNode* f = def->fields->data.expression_list.expressions[i];
        if (f && f->type == AST_ASSIGN && f->data.assign.left && f->data.assign.left->type == AST_IDENTIFIER) {
            const char* fname = f->data.assign.left->data.identifier.name;
            int d = levenshtein_distance(field_name, fname);
            if (d < bestd) { bestd = d; best = fname; }
        }
    }
    if (bestd <= 2) return best;
    return NULL;
}
static StructDef* find_struct_definition(const char* name) {
    StructDef* current = g_struct_definitions;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}
static int add_struct_definition(const char* name, ASTNode* fields) {
    if (!name) return 0;
    
    StructDef* existing = find_struct_definition(name);
    if (existing) {// 结构体已存在 不重复添加
        return 0;
    }
    
    StructDef* new_def = malloc(sizeof(StructDef));
    if (!new_def) return 0;
    
    new_def->name = malloc(strlen(name) + 1);
    if (!new_def->name) {
        free(new_def);
        return 0;
    }
    strcpy(new_def->name, name);
    new_def->fields = fields;
    new_def->next = g_struct_definitions;
    g_struct_definitions = new_def;
    
    return 1;
}

SymbolTable* create_symbol_table(SymbolTable* parent) {
    SymbolTable* table = malloc(sizeof(SymbolTable));
    if (!table) return NULL;
    table->head = NULL;
    table->parent = parent;
    return table;
}

static Symbol* create_symbol(const char* name, SymbolType type, InferredType inferred_type) {
    if (!name) return NULL;
    Symbol* sym = malloc(sizeof(Symbol));
    if (!sym) return NULL;
    
    size_t name_len = strlen(name);
    sym->name = malloc(name_len + 1);
    if (!sym->name) {
        free(sym);
        return NULL;
    }
    strcpy(sym->name, name);
    
    sym->type = type;
    sym->inferred_type = inferred_type;
    sym->is_mutable_pointer = 0;
    sym->next = NULL;
    
    return sym;
}

int add_symbol(SymbolTable* table, const char* name, SymbolType type, InferredType inferred_type) {
    return add_symbol_with_mutability(table, name, type, inferred_type, 0);
}

int add_symbol_with_mutability(SymbolTable* table, const char* name, SymbolType type, InferredType inferred_type, int is_mutable_pointer) {
    if (!table || !name) return 0;
    
    Symbol* sym = create_symbol(name, type, inferred_type);
    if (!sym) return 0;
    
    sym->is_mutable_pointer = is_mutable_pointer;
    sym->next = table->head;
    table->head = sym;
    
    return 1;
}

Symbol* lookup_symbol(SymbolTable* table, const char* name) {
    if (!table || !name) return NULL;
    
    Symbol* current = table->head;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    if (table->parent) {
        return lookup_symbol(table->parent, name);
    }
    
    return NULL;
}

static Symbol* lookup_symbol_current_scope(SymbolTable* table, const char* name) {
    if (!table || !name) return NULL;
    
    Symbol* current = table->head;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

void destroy_symbol_table(SymbolTable* symbol_table) {
    if (!symbol_table) return;
    
    Symbol* current = symbol_table->head;
    while (current) {
        Symbol* next = current->next;
        free(current->name);
        free(current);
        current = next;
    }
    
    free(symbol_table);
}

static int is_node_visited(ASTNode* node, VisitedNode* visited_list) {
    VisitedNode* current = visited_list;
    while (current) {
        if (current->node == node) {
            return 1; 
        }
        current = current->next;
    }
    return 0;
}

static VisitedNode* add_visited_node(ASTNode* node, VisitedNode* visited_list) {
    VisitedNode* new_visited = malloc(sizeof(VisitedNode));
    if (!new_visited) return visited_list;
    new_visited->node = node;
    new_visited->next = visited_list;
    return new_visited;
}

/* static VisitedNode* remove_visited_node(ASTNode* node, VisitedNode* visited_list) {
    if (!visited_list) return NULL;
    
    if (visited_list->node == node) {
        VisitedNode* next = visited_list->next;
        free(visited_list);
        return next;
    }
    return visited_list;
} */

static int is_lvalue_mutable(ASTNode* node, SymbolTable* table) {
    if (!node) return 0;
    
    if (node->type == AST_IDENTIFIER) {
        Symbol* sym = lookup_symbol(table, node->data.identifier.name);
        if (sym && sym->is_mutable_pointer) {
            return 1;
        }
    } else if (node->type == AST_UNARYOP && node->data.unaryop.op == OP_DEREF) {
        return is_lvalue_mutable(node->data.unaryop.expr, table);
    } else if (node->type == AST_INDEX) {
        return is_lvalue_mutable(node->data.index.target, table);
    }
    
    return 0;
}

static int check_undefined_symbols_in_node_with_visited(ASTNode* node, SymbolTable* table, VisitedNode* visited_list) {
    if (!node) return 0;
    if (is_node_visited(node, visited_list)) {
        return 0;
    }
    VisitedNode* new_visited_list = add_visited_node(node, visited_list);
    if (!new_visited_list) {
        /*
        如果无法添加到访问列表，可能是因为内存分配失败
        在这种情况下，我们仍然可以继续处理，但不进行递归保护
        但为了安全，我们返回0避免进一步处理
        */
        return 0;
    }
    
    int errors_found = 0;
    
    switch (node->type) {
        case AST_PROGRAM: {
            SymbolTable* func_table = create_symbol_table(table);
            for (int i = 0; i < node->data.program.statement_count; i++) {
                ASTNode* stmt = node->data.program.statements[i];
                if (stmt && stmt->type == AST_FUNCTION) {
                    add_symbol(table, stmt->data.function.name, SYMBOL_FUNCTION, TYPE_UNKNOWN);
                    add_symbol(func_table, stmt->data.function.name, SYMBOL_FUNCTION, TYPE_UNKNOWN);
                }
            }
            for (int i = 0; i < node->data.program.statement_count; i++) {
                ASTNode* stmt = node->data.program.statements[i];
                if (stmt && stmt->type == AST_GLOBAL) {
                    ASTNode* identifier = stmt->data.global_decl.identifier;
                    if (identifier && identifier->type == AST_IDENTIFIER) {
                        const char* var_name = identifier->data.identifier.name;
                        if (lookup_symbol(func_table, var_name)) {
                            const char* filename = current_input_filename ? current_input_filename : "unknown";
                            int line = (identifier->location.first_line > 0) ? identifier->location.first_line : 1;
                            char buf[256];
                            snprintf(buf, sizeof(buf), "Global variable '%s' conflicts with function name", var_name);
                            report_semantic_error_with_location(buf, filename, line);
                            errors_found++;
                            continue;
                        }
                        add_symbol(table, var_name, SYMBOL_VARIABLE, TYPE_UNKNOWN);
                        if (stmt->data.global_decl.initializer) {
                            add_var_init_mapping(var_name, stmt->data.global_decl.initializer);
                        }
                    }
                }
            }
            for (int i = 0; i < node->data.program.statement_count; i++) {
                ASTNode* stmt = node->data.program.statements[i];
                if (stmt && stmt->type == AST_CONST) {
                    ASTNode* left = stmt->data.assign.left;
                    if (left && left->type == AST_IDENTIFIER) {
                        const char* name = left->data.identifier.name;
                        if (name && !lookup_symbol(table, name)) {
                            add_symbol(table, name, SYMBOL_CONSTANT, TYPE_UNKNOWN);
                        }
                    }
                }
            }
            destroy_symbol_table(func_table);
            for (int i = 0; i < node->data.program.statement_count; i++) {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.program.statements[i], table, new_visited_list);
            }
            break;
        }
        
        case AST_ASSIGN: {
            int in_struct_def_field = is_node_struct_field_assignment(node, new_visited_list);
            int in_struct_literal_field = is_node_inside_struct_literal(node, new_visited_list);
            int is_type_annotation = (node->data.assign.is_declaration == 2);

            if (node->data.assign.left && 
                node->data.assign.left->type == AST_UNARYOP && 
                node->data.assign.left->data.unaryop.op == OP_DEREF) {
                
                if (!is_lvalue_mutable(node->data.assign.left->data.unaryop.expr, table)) {
                    const char* filename = current_input_filename ? current_input_filename : "unknown";
                    int line = (node->data.assign.left->location.first_line > 0) ? node->data.assign.left->location.first_line : 1;
                    char buf[512];
                    snprintf(buf, sizeof(buf), 
                        "Cannot assign to dereference of an immutable pointer\n"
                        "Fix: Declare the pointer as mutable using 'mut ptr = &var')");
                    report_semantic_error_with_location(buf, filename, line);
                    errors_found++;
                }
            }

            // 检查左值（left-hand side）
            if (node->data.assign.left && node->data.assign.left->type == AST_IDENTIFIER) {
                Symbol* existing = lookup_symbol(table, node->data.assign.left->data.identifier.name);
                if (in_struct_def_field || in_struct_literal_field) {
                    /* 结构体字段定义/初始化中的 left 是字段名，不是变量声明或赋值目标 */
                } else if (existing && existing->type == SYMBOL_CONSTANT) {
                    const char* filename = current_input_filename ? current_input_filename : "unknown";
                    int line = (node->data.assign.left->location.first_line > 0) ? node->data.assign.left->location.first_line : 1;
                    char buf[256];
                    snprintf(buf, sizeof(buf), "cannot assign to constant: '%s'", node->data.assign.left->data.identifier.name);
                    report_semantic_error_with_location(buf, filename, line);
                    errors_found++;
                } else {
                    if (node->data.assign.is_declaration) {
                        if (existing && lookup_symbol_current_scope(table, node->data.assign.left->data.identifier.name)) {
                            const char* filename = current_input_filename ? current_input_filename : "unknown";
                            int line = (node->data.assign.left->location.first_line > 0) ? node->data.assign.left->location.first_line : 1;
                            report_redefinition_error_with_location(node->data.assign.left->data.identifier.name, filename, line);
                            errors_found++;
                        } else {
                            int is_mutable = (node->data.assign.left->mutability == MUTABILITY_MUTABLE);
                            add_symbol_with_mutability(table, node->data.assign.left->data.identifier.name, SYMBOL_VARIABLE, TYPE_UNKNOWN, is_mutable);
                            if (node->data.assign.right && node->data.assign.right->type == AST_STRUCT_LITERAL &&
                                node->data.assign.right->data.struct_literal.type_name &&
                                node->data.assign.right->data.struct_literal.type_name->type == AST_IDENTIFIER) {
                                const char* sname = node->data.assign.right->data.struct_literal.type_name->data.identifier.name;
                                add_var_struct_mapping(node->data.assign.left->data.identifier.name, sname);
                            }
                            if (node->data.assign.right) {
                                add_var_init_mapping(node->data.assign.left->data.identifier.name, node->data.assign.right);
                            }
                        }
                    } else {
                        if (!existing) {
                            const char* filename = current_input_filename ? current_input_filename : "unknown";
                            int line = (node->data.assign.left->location.first_line > 0) ? node->data.assign.left->location.first_line : 1;
                            char buf[256];
                            snprintf(buf, sizeof(buf), "Variable '%s' must be declared with 'let' before assignment", node->data.assign.left->data.identifier.name);
                            report_semantic_error_with_location(buf, filename, line);
                            errors_found++;
                        } else if (!existing->is_mutable_pointer && existing->type == SYMBOL_VARIABLE) {
                            const char* filename = current_input_filename ? current_input_filename : "unknown";
                            int line = (node->data.assign.left->location.first_line > 0) ? node->data.assign.left->location.first_line : 1;
                            char buf[256];
                            snprintf(buf, sizeof(buf),
                                "cannot assign to immutable variable '%s'\n"
                                "  Fix: declare with 'let mut' to make it mutable",
                                node->data.assign.left->data.identifier.name);
                            report_semantic_error_with_location(buf, filename, line);
                            errors_found++;
                        } else if (node->data.assign.right) {
                            add_var_init_mapping(node->data.assign.left->data.identifier.name, node->data.assign.right);
                        }
                    }
                }
            }
            if (node->data.assign.right) {
                if (is_type_annotation) {
                    break;
                }
                if (in_struct_def_field) {
                    /*
                    结构体字段的右侧是类型注解，允许泛型参数、联合类型、函数类型、元组类型等。
                    这里不按“变量/函数标识符必须已定义”规则检查，避免误报。
                    */
                } else {
                    errors_found += check_undefined_symbols_in_node_with_visited(node->data.assign.right, table, new_visited_list);
                }
            }
            break;
        }

        case AST_CONST: {
            errors_found += check_undefined_symbols_in_node_with_visited(node->data.assign.right, table, new_visited_list);
            if (node->data.assign.left && node->data.assign.left->type == AST_IDENTIFIER) {
                Symbol* existing = lookup_symbol(table, node->data.assign.left->data.identifier.name);
                if (existing && existing->type != SYMBOL_CONSTANT) {
                    const char* filename = current_input_filename ? current_input_filename : "unknown";
                    int line = (node->data.assign.left->location.first_line > 0) ? node->data.assign.left->location.first_line : 1;
                    report_redefinition_error_with_location(node->data.assign.left->data.identifier.name, filename, line);
                    errors_found++;
                } else if (!existing) {
                    add_symbol(table, node->data.assign.left->data.identifier.name, SYMBOL_CONSTANT, TYPE_UNKNOWN);
                    if (node->data.assign.right && node->data.assign.right->type == AST_STRUCT_LITERAL &&
                        node->data.assign.right->data.struct_literal.type_name &&
                        node->data.assign.right->data.struct_literal.type_name->type == AST_IDENTIFIER) {
                        const char* sname = node->data.assign.right->data.struct_literal.type_name->data.identifier.name;
                        add_var_struct_mapping(node->data.assign.left->data.identifier.name, sname);
                    }
                }
            }
            break;
        }
        
        case AST_IDENTIFIER: {
            if (is_builtin_union_ctor_name(node->data.identifier.name)) {
                break;
            }
            Symbol* sym = lookup_symbol(table, node->data.identifier.name);
            if (!sym) {
                const char* filename = node_source_filename(node);
                int line = (node->location.first_line > 0) ? node->location.first_line : 1;
                int column = (node->location.first_column > 0) ? node->location.first_column : 1;
                report_undefined_identifier_with_location_and_column(
                    node->data.identifier.name, 
                    filename, 
                    line,
                    column);
                errors_found++;
            }
            break;
        }
        
        case AST_FUNCTION: {
            add_symbol(table, node->data.function.name, SYMBOL_FUNCTION, TYPE_UNKNOWN);
            SymbolTable* func_scope = create_symbol_table(table);
            if (node->data.function.params && node->data.function.params->type == AST_EXPRESSION_LIST) {
                for (int i = 0; i < node->data.function.params->data.expression_list.expression_count; i++) {
                    ASTNode* param = node->data.function.params->data.expression_list.expressions[i];
                    if (param->type == AST_IDENTIFIER) {
                        add_symbol(func_scope, param->data.identifier.name, SYMBOL_VARIABLE, TYPE_UNKNOWN);
                    }
                    else if (param->type == AST_ASSIGN && param->data.assign.left->type == AST_IDENTIFIER) {
                        int is_mut = 0;
                        if (param->mutability == MUTABILITY_MUTABLE) is_mut = 1;
                        add_symbol_with_mutability(func_scope, param->data.assign.left->data.identifier.name, SYMBOL_VARIABLE, TYPE_UNKNOWN, is_mut);
                    }
                }
            }
            if (node->data.function.body) {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.function.body, func_scope, new_visited_list);
            }
            destroy_symbol_table(func_scope);
            break;
        }
        
        case AST_CALL: {
            if (node->data.call.func && node->data.call.func->type == AST_IDENTIFIER) {
                if (is_builtin_union_ctor_name(node->data.call.func->data.identifier.name)) {
                    if (node->data.call.args) {
                        errors_found += check_undefined_symbols_in_node_with_visited(node->data.call.args, table, new_visited_list);
                    }
                    break;
                }
                Symbol* sym = lookup_symbol(table, node->data.call.func->data.identifier.name);
                if (!sym) {
                    const char* filename = node_source_filename(node->data.call.func);
                    int line = (node->data.call.func->location.first_line > 0) ? node->data.call.func->location.first_line : 1;
                    int column = (node->data.call.func->location.first_column > 0) ? node->data.call.func->location.first_column : 1;
                    report_undefined_function_with_location_and_column(
                        node->data.call.func->data.identifier.name, 
                        filename, 
                        line,
                        column);
                    errors_found++;
                }
            }
            if (node->data.call.args) {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.call.args, table, new_visited_list);
            }
            break;
        }
        case AST_STRUCT_DEF: {
            add_struct_definition(node->data.struct_def.name, node->data.struct_def.fields);
            if (node->data.struct_def.fields) {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.struct_def.fields, table, new_visited_list);
            }
            break;
        }
        case AST_STRUCT_LITERAL: {// 检查结构体字面量类型名是否已定义
            if (node->data.struct_literal.type_name && node->data.struct_literal.type_name->type == AST_IDENTIFIER) {
                const char* struct_name = node->data.struct_literal.type_name->data.identifier.name;
                StructDef* struct_def = find_struct_definition(struct_name);
                if (!struct_def) {
                    const char* filename = node_source_filename(node->data.struct_literal.type_name);
                    int line = (node->data.struct_literal.type_name->location.first_line > 0) ? node->data.struct_literal.type_name->location.first_line : 1;
                    report_undefined_identifier_with_location_and_column(struct_name, filename, line, 1);
                    errors_found++;
                }
            }
            if (node->data.struct_literal.fields) {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.struct_literal.fields, table, new_visited_list);
            }
            break;
        }
        case AST_INDEX: {
            errors_found += check_undefined_symbols_in_node_with_visited(node->data.index.target, table, new_visited_list);
            /*如果是结构体字段访问，我们不应该检查字段名是否为标识符
            而是应该检查字段名是否是结构体的有效字段*/
            if (node->data.index.index && node->data.index.index->type == AST_IDENTIFIER) {
                // like obj.field
                const char* field_name = node->data.index.index->data.identifier.name;
                const char* struct_name = NULL;
                if (node->data.index.target && node->data.index.target->type == AST_STRUCT_LITERAL) {
                    if (node->data.index.target->data.struct_literal.type_name &&
                        node->data.index.target->data.struct_literal.type_name->type == AST_IDENTIFIER) {
                        struct_name = node->data.index.target->data.struct_literal.type_name->data.identifier.name;
                    }
                }
                if (!struct_name && node->data.index.target && node->data.index.target->type == AST_IDENTIFIER) {
                    const char* varname = node->data.index.target->data.identifier.name;
                    struct_name = find_var_struct_mapping(varname);
                }

                if (struct_name) {
                    StructDef* def = find_struct_definition(struct_name);
                    if (def) {
                        ASTNode* found = find_field_in_struct(def, field_name);
                        if (!found) {
                            const char* filename = current_input_filename ? current_input_filename : "unknown";
                            int line = (node->data.index.index->location.first_line > 0) ? node->data.index.index->location.first_line : 1;
                            const char* suggestion = find_closest_field_name(def, field_name);
                            char buf[512];
                            if (suggestion) {
                                snprintf(buf, sizeof(buf),
                                    "Field '%s' does not exist in struct '%s'.\n"
                                    "Did you mean is '%s'?", 
                                    field_name, struct_name, suggestion);
                            } else {
                                snprintf(buf, sizeof(buf),
                                    "Field '%s' does not exist in struct '%s'.", 
                                    field_name, struct_name);
                            }
                            report_semantic_error_with_location(buf, filename, line);
                            errors_found++;
                        }
                    }
                }
            } else {
                if (node->data.index.index && node->data.index.index->type == AST_NUM_INT) {
                    long long index_value = node->data.index.index->data.num_int.value;
                    if (node->data.index.target && node->data.index.target->type == AST_EXPRESSION_LIST) {
                        int array_length = node->data.index.target->data.expression_list.expression_count;
                        if (index_value >= array_length) {
                            const char* filename = current_input_filename ? current_input_filename : "unknown";
                            int line = (node->data.index.index->location.first_line > 0) ? node->data.index.index->location.first_line : 1;
                            const char* array_name = NULL;
                            if (node->data.index.target->location.first_line > 0) {
                                array_name = "[...]";
                            }
                            report_array_out_of_bounds_error_with_location(array_name, (int)index_value, array_length, filename, line);
                            errors_found++;
                        }
                    }
                    else if (node->data.index.target && node->data.index.target->type == AST_IDENTIFIER) {
                        const char* var_name = node->data.index.target->data.identifier.name;
                        ASTNode* init_value = find_var_init_mapping(var_name);
                        if (init_value && init_value->type == AST_EXPRESSION_LIST) {
                            int array_length = init_value->data.expression_list.expression_count;
                            /* Skip bounds check for empty arrays (may be modified by push at runtime) */
                            if (array_length > 0 && index_value >= array_length) {
                                const char* filename = current_input_filename ? current_input_filename : "unknown";
                                int line = (node->data.index.index->location.first_line > 0) ? node->data.index.index->location.first_line : 1;
                                report_array_out_of_bounds_error_with_location(var_name, (int)index_value, array_length, filename, line);
                                errors_found++;
                            }
                        }
                    }
                }
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.index.index, table, new_visited_list);
            }
            break;
        }
        
        case AST_MEMBER_ACCESS: {
            errors_found += check_undefined_symbols_in_node_with_visited(node->data.member_access.object, table, new_visited_list);
            /*
            处理结构体字段访问，检查字段名是否为标识符
            并验证字段名是否是结构体的有效字段
            */
            if (node->data.member_access.field && node->data.member_access.field->type == AST_IDENTIFIER) {
                // like obj.field
                const char* field_name = node->data.member_access.field->data.identifier.name;
                const char* struct_name = NULL;
                if (node->data.member_access.object && node->data.member_access.object->type == AST_STRUCT_LITERAL) {
                    if (node->data.member_access.object->data.struct_literal.type_name &&
                        node->data.member_access.object->data.struct_literal.type_name->type == AST_IDENTIFIER) {
                        struct_name = node->data.member_access.object->data.struct_literal.type_name->data.identifier.name;
                    }
                }
                if (!struct_name && node->data.member_access.object && node->data.member_access.object->type == AST_IDENTIFIER) {
                    const char* varname = node->data.member_access.object->data.identifier.name;
                    struct_name = find_var_struct_mapping(varname);
                }

                if (struct_name) {
                    StructDef* def = find_struct_definition(struct_name);
                    if (def) {
                        ASTNode* found = find_field_in_struct(def, field_name);
                        if (!found) {
                            const char* filename = current_input_filename ? current_input_filename : "unknown";
                            int line = (node->data.member_access.field->location.first_line > 0) ? node->data.member_access.field->location.first_line : 1;
                            const char* suggestion = find_closest_field_name(def, field_name);
                            char buf[512];
                            if (suggestion) {
                                snprintf(buf, sizeof(buf),
                                    "Field '%s' does not exist in struct '%s'.\n"
                                    "Did you mean is '%s'?", 
                                    field_name, struct_name, suggestion);
                            } else {
                                snprintf(buf, sizeof(buf),
                                    "Field '%s' does not exist in struct '%s'.", 
                                    field_name, struct_name);
                            }
                            report_semantic_error_with_location(buf, filename, line);
                            errors_found++;
                        }
                    }
                }
            } else {
                if (node->data.member_access.field) {
                    errors_found += check_undefined_symbols_in_node_with_visited(node->data.member_access.field, table, new_visited_list);
                }
            }
            break;
        }
        
        case AST_BINOP:
        case AST_UNARYOP: {
            if (node->type == AST_BINOP) {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.binop.left, table, new_visited_list);
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.binop.right, table, new_visited_list);
            } else {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.unaryop.expr, table, new_visited_list);
            }
            break;
        }
        
        
        case AST_IF: {
            errors_found += check_undefined_symbols_in_node_with_visited(node->data.if_stmt.condition, table, new_visited_list);
            SymbolTable* then_scope = create_symbol_table(table);
            if (then_scope) {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.if_stmt.then_body, then_scope, new_visited_list);
                destroy_symbol_table(then_scope);
            } else {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.if_stmt.then_body, table, new_visited_list);
            }
            if (node->data.if_stmt.else_body) {
                SymbolTable* else_scope = create_symbol_table(table);
                if (else_scope) {
                    errors_found += check_undefined_symbols_in_node_with_visited(node->data.if_stmt.else_body, else_scope, new_visited_list);
                    destroy_symbol_table(else_scope);
                } else {
                    errors_found += check_undefined_symbols_in_node_with_visited(node->data.if_stmt.else_body, table, new_visited_list);
                }
            }
            break;
        }
        
        case AST_WHILE: {
            errors_found += check_undefined_symbols_in_node_with_visited(node->data.while_stmt.condition, table, new_visited_list);
            errors_found += check_undefined_symbols_in_node_with_visited(node->data.while_stmt.body, table, new_visited_list);
            break;
        }
        
        case AST_FOR: {
            errors_found += check_undefined_symbols_in_node_with_visited(node->data.for_stmt.start, table, new_visited_list);
            errors_found += check_undefined_symbols_in_node_with_visited(node->data.for_stmt.end, table, new_visited_list);
            if (node->data.for_stmt.var && node->data.for_stmt.var->type == AST_IDENTIFIER) {
                add_symbol(table, node->data.for_stmt.var->data.identifier.name, SYMBOL_VARIABLE, TYPE_UNKNOWN);
            }
            errors_found += check_undefined_symbols_in_node_with_visited(node->data.for_stmt.body, table, new_visited_list);
            break;
        }
        
        case AST_PRINT: {
            errors_found += check_undefined_symbols_in_node_with_visited(node->data.print.expr, table, new_visited_list);
            break;
        }
        
        case AST_INPUT: {
            if (node->data.input.prompt) {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.input.prompt, table, new_visited_list);
            }
            break;
        }
        
        case AST_TOINT:
        case AST_TOFLOAT: {
            if (node->type == AST_TOINT) {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.toint.expr, table, new_visited_list);
            } else {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.tofloat.expr, table, new_visited_list);
            }
            break;
        }
        
        case AST_RETURN: {
            if (node->data.return_stmt.expr) {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.return_stmt.expr, table, new_visited_list);
            }
            break;
        }
        
        case AST_IMPORT: {
            const char* module_path = node->data.import.module_path;
            char full_module_path[1024];
            if (!vix_resolve_import_path(current_input_filename, module_path, full_module_path, sizeof(full_module_path)))
            {
                const char* filename = current_input_filename ? current_input_filename : "unknown";
                int line = (node->location.first_line > 0) ? node->location.first_line : 1;
                char error_msg[512];
                snprintf(error_msg, sizeof(error_msg), "Module file not found: %s", module_path);
                report_semantic_error_with_location(error_msg, filename, line);
                errors_found++;
            }
            else 
            {
                errors_found += extract_public_functions_from_module(full_module_path, table);
            }
            break;
        }
        
        case AST_EXPRESSION_LIST: {
            for (int i = 0; i < node->data.expression_list.expression_count; i++)
            {
                errors_found += check_undefined_symbols_in_node_with_visited(node->data.expression_list.expressions[i], table, new_visited_list);
            }
            break;
        }
        
        case AST_NUM_INT:
        case AST_NUM_FLOAT:
        case AST_STRING:
            break;
            
        default:
            break;
    }
    
    /*释放当前添加的访问标记节点，但不释放整个访问列表
    因为访问列表是由调用者管理的*/
    VisitedNode* to_free = new_visited_list;
    new_visited_list = new_visited_list->next;
    free(to_free);
    
    return errors_found;
}

int check_undefined_symbols(ASTNode* node) {
    StructDef* current = g_struct_definitions;
    while (current) {
        StructDef* next = current->next;
        free(current->name);
        free(current);
        current = next;
    }
    g_struct_definitions = NULL;
    clear_var_struct_map();
    clear_var_init_map(); // 清理初始化映射
    
    SymbolTable* global_table = create_symbol_table(NULL);
    if (!global_table) return 1;
    int result = check_undefined_symbols_in_node_with_visited(node, global_table, NULL);
    destroy_symbol_table(global_table);
    return result;
}

int is_variable_used_in_node(ASTNode* node, const char* var_name) {
    if (!node || !var_name) return 0;
    
    switch (node->type) {
        case AST_PROGRAM: {
            for (int i = 0; i < node->data.program.statement_count; i++) {
                if (is_variable_used_in_node(node->data.program.statements[i], var_name)) {
                    return 1;
                }
            }
            break;
        }
        
        case AST_ASSIGN: {
            if (is_variable_used_in_node(node->data.assign.right, var_name)) { //右边
                return 1;
            }
            
            if (node->data.assign.left && node->data.assign.left->type == AST_IDENTIFIER) {
                if (strcmp(node->data.assign.left->data.identifier.name, var_name) == 0) {
                    return 0; // 这是赋值 不是使用 如果左边是相同的变量名 则不是使用而是赋只
                }
            }
            break;
        }
        
        case AST_IDENTIFIER: {
            return strcmp(node->data.identifier.name, var_name) == 0;
        }
        
        case AST_CALL: {
            if (node->data.call.args) {
                if (is_variable_used_in_node(node->data.call.args, var_name)) {
                    return 1;
                }
            }
            break;
        }
        
        case AST_BINOP:
        case AST_UNARYOP: {
            if (node->type == AST_BINOP) {
                return is_variable_used_in_node(node->data.binop.left, var_name) ||is_variable_used_in_node(node->data.binop.right, var_name);
            } else {
                return is_variable_used_in_node(node->data.unaryop.expr, var_name);
            }
        }
        
        case AST_IF: {
            if (is_variable_used_in_node(node->data.if_stmt.condition, var_name)) {
                return 1;
            }
            if (is_variable_used_in_node(node->data.if_stmt.then_body, var_name)) {
                return 1;
            }
            if (node->data.if_stmt.else_body && 
                is_variable_used_in_node(node->data.if_stmt.else_body, var_name)) {
                return 1;
            }
            break;
        }
        
        case AST_WHILE: {
            if (is_variable_used_in_node(node->data.while_stmt.condition, var_name)) {
                return 1;
            }
            if (is_variable_used_in_node(node->data.while_stmt.body, var_name)) {
                return 1;
            }
            break;
        }
        
        case AST_FOR: {
            if (is_variable_used_in_node(node->data.for_stmt.start, var_name)) {
                return 1;
            }
            if (is_variable_used_in_node(node->data.for_stmt.end, var_name)) {
                return 1;
            }
            if (node->data.for_stmt.var && node->data.for_stmt.var->type == AST_IDENTIFIER) {
                if (strcmp(node->data.for_stmt.var->data.identifier.name, var_name) == 0) {
                    return 0; // 循环变量定义 不算使用
                }
            }
            if (is_variable_used_in_node(node->data.for_stmt.body, var_name)) {
                return 1;
            }
            break;
        }
        
        case AST_PRINT: {
            return is_variable_used_in_node(node->data.print.expr, var_name);
        }
        
        case AST_INPUT: {
            if (node->data.input.prompt) {
                return is_variable_used_in_node(node->data.input.prompt, var_name);
            }
            break;
        }
        
        case AST_TOINT:
        case AST_TOFLOAT: {
            if (node->type == AST_TOINT) {
                return is_variable_used_in_node(node->data.toint.expr, var_name);
            } else {
                return is_variable_used_in_node(node->data.tofloat.expr, var_name);
            }
        }
        
        case AST_RETURN: {
            if (node->data.return_stmt.expr) {
                return is_variable_used_in_node(node->data.return_stmt.expr, var_name);
            }
            break;
        }
        
        case AST_EXPRESSION_LIST: {
            for (int i = 0; i < node->data.expression_list.expression_count; i++) {
                if (is_variable_used_in_node(node->data.expression_list.expressions[i], var_name)) {
                    return 1;
                }
            }
            break;
        }

        case AST_STRUCT_LITERAL: {
            ASTNode* fields = node->data.struct_literal.fields;
            if (fields && fields->type == AST_EXPRESSION_LIST) {
                for (int i = 0; i < fields->data.expression_list.expression_count; i++) {
                    ASTNode* field = fields->data.expression_list.expressions[i];
                    if (field && field->type == AST_ASSIGN) {
                        if (is_variable_used_in_node(field->data.assign.right, var_name)) {
                            return 1;
                        }
                    } else if (is_variable_used_in_node(field, var_name)) {
                        return 1;
                    }
                }
            }
            break;
        }

        case AST_INDEX: {
            return is_variable_used_in_node(node->data.index.target, var_name) || is_variable_used_in_node(node->data.index.index, var_name);
        }
        
        case AST_MEMBER_ACCESS: {
            return is_variable_used_in_node(node->data.member_access.object, var_name) || is_variable_used_in_node(node->data.member_access.field, var_name);
        }
        
        case AST_NUM_INT:
        case AST_NUM_FLOAT:
        case AST_STRING:
            break;
            
        default:
            break;
    }
    
    return 0;
}
typedef struct VariableUsage {
    char* name;
    int used;
    int line;
    int column;
    struct VariableUsage* next;
} VariableUsage;
VariableUsage* add_variable_to_usage_with_column(VariableUsage* list, const char* name, int line, int column) {
    VariableUsage* new_var = malloc(sizeof(VariableUsage));
    new_var->name = malloc(strlen(name) + 1);
    strcpy(new_var->name, name);
    new_var->used = 0;
    new_var->line = line;
    new_var->column = column;
    new_var->next = list;
    return new_var;
}

VariableUsage* add_variable_to_usage(VariableUsage* list, const char* name, int line) {
    return add_variable_to_usage_with_column(list, name, line, 1);
}
VariableUsage* find_variable_in_usage(VariableUsage* list, const char* name) {
    VariableUsage* current = list;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}
void free_variable_usage(VariableUsage* list) {
    while (list) {
        VariableUsage* temp = list;
        list = list->next;
        free(temp->name);
        free(temp);
    }
}
int check_unused_variables(ASTNode* node, SymbolTable* table) {
    VariableUsage* usage_list = NULL;
    int warnings_found = 0;
    warnings_found = check_unused_variables_with_usage(node, table, &usage_list);
    VariableUsage* current = usage_list;
    while (current) {
        if (!current->used) {
            const char* filename = current_input_filename ? current_input_filename : "unknown";
            report_unused_variable_warning_with_location(current->name, filename, current->line, current->column);
            warnings_found++;
        }
        current = current->next;
    }
    
    free_variable_usage(usage_list);
    return warnings_found;
}
int check_unused_variables_with_usage(ASTNode* node, SymbolTable* table, struct VariableUsage** usage_list) {
    if (!node) return 0;
    
    int warnings_found = 0;
    
    switch (node->type) {
        case AST_PROGRAM: {
            for (int i = 0; i < node->data.program.statement_count; i++) {
                warnings_found += check_unused_variables_with_usage(node->data.program.statements[i], table, usage_list);
            }
            break;
        }
        
        case AST_ASSIGN: {
            warnings_found += check_unused_variables_with_usage(node->data.assign.right, table, usage_list);
            if (node->data.assign.left && node->data.assign.left->type == AST_IDENTIFIER) {
                int line = (node->data.assign.left->location.first_line > 0) ? node->data.assign.left->location.first_line : 1;
                int column = (node->data.assign.left->location.first_column > 0) ? node->data.assign.left->location.first_column : 1;
                add_symbol(table, node->data.assign.left->data.identifier.name, SYMBOL_VARIABLE, TYPE_UNKNOWN);
                VariableUsage* var_usage = find_variable_in_usage(*usage_list, node->data.assign.left->data.identifier.name);
                if (!var_usage) {
                    VariableUsage* new_usage = add_variable_to_usage_with_column(*usage_list, node->data.assign.left->data.identifier.name, line, column);
                    *usage_list = new_usage;
                }
            }
            break;
        }
        case AST_CONST: {
            warnings_found += check_unused_variables_with_usage(node->data.assign.right, table, usage_list);
            if (node->data.assign.left && node->data.assign.left->type == AST_IDENTIFIER) {
                int line = (node->data.assign.left->location.first_line > 0) ? node->data.assign.left->location.first_line : 1;
                int column = (node->data.assign.left->location.first_column > 0) ? node->data.assign.left->location.first_column : 1;
                add_symbol(table, node->data.assign.left->data.identifier.name, SYMBOL_CONSTANT, TYPE_UNKNOWN);
                VariableUsage* var_usage = find_variable_in_usage(*usage_list, node->data.assign.left->data.identifier.name);
                if (!var_usage) {
                    VariableUsage* new_usage = add_variable_to_usage_with_column(*usage_list, node->data.assign.left->data.identifier.name, line, column);
                    *usage_list = new_usage;
                }
            }
            break;
        }
        
        case AST_IDENTIFIER: {
            Symbol* sym = lookup_symbol(table, node->data.identifier.name);
            if (sym) {
                VariableUsage* var_usage = find_variable_in_usage(*usage_list, node->data.identifier.name);
                if (var_usage) {
                    var_usage->used = 1;
                }
            }
            break;
        }
        
        case AST_FUNCTION: {
            int line = (node->location.first_line > 0) ? node->location.first_line : 1;
            int column = (node->location.first_column > 0) ? node->location.first_column : 1;
            
            add_symbol(table, node->data.function.name, SYMBOL_FUNCTION, TYPE_UNKNOWN);
            SymbolTable* func_scope = create_symbol_table(table);
            int should_track_params = !(node->data.function.is_extern && node->data.function.body == NULL);
            if (should_track_params && node->data.function.params && node->data.function.params->type == AST_EXPRESSION_LIST) {
                for (int i = 0; i < node->data.function.params->data.expression_list.expression_count; i++) {
                    ASTNode* param = node->data.function.params->data.expression_list.expressions[i];
                    if (param->type == AST_IDENTIFIER) {
                        add_symbol(func_scope, param->data.identifier.name, SYMBOL_VARIABLE, TYPE_UNKNOWN);
                        VariableUsage* var_usage = find_variable_in_usage(*usage_list, param->data.identifier.name);
                        if (!var_usage) {
                            int param_line = (param->location.first_line > 0) ? param->location.first_line : line;
                            int param_column = (param->location.first_column > 0) ? param->location.first_column : column;
                            VariableUsage* new_usage = add_variable_to_usage_with_column(*usage_list, param->data.identifier.name, param_line, param_column);
                            *usage_list = new_usage;
                        }
                    }
                    else if (param->type == AST_ASSIGN && param->data.assign.left->type == AST_IDENTIFIER) {
                        int is_mut = 0;
                        if (param->mutability == MUTABILITY_MUTABLE) is_mut = 1;
                        add_symbol_with_mutability(func_scope, param->data.assign.left->data.identifier.name, SYMBOL_VARIABLE, TYPE_UNKNOWN, is_mut);
                        VariableUsage* var_usage = find_variable_in_usage(*usage_list, param->data.assign.left->data.identifier.name);
                        if (!var_usage) {
                            int param_line = (param->location.first_line > 0) ? param->location.first_line : line;
                            int param_column = (param->location.first_column > 0) ? param->location.first_column : column;
                            VariableUsage* new_usage = add_variable_to_usage_with_column(*usage_list, param->data.assign.left->data.identifier.name, param_line, param_column);
                            *usage_list = new_usage;
                        }
                    }
                }
            }
            if (node->data.function.body) {
                warnings_found += check_unused_variables_with_usage(node->data.function.body, func_scope, usage_list);
            }
            destroy_symbol_table(func_scope);
            break;
        }
        
        case AST_CALL: {
            if (node->data.call.func && node->data.call.func->type == AST_IDENTIFIER) {
                Symbol* sym = lookup_symbol(table, node->data.call.func->data.identifier.name);
                if (sym) {
                    VariableUsage* var_usage = find_variable_in_usage(*usage_list, node->data.call.func->data.identifier.name);
                    if (var_usage) {
                        var_usage->used = 1;
                    }
                }
            }
            if (node->data.call.args) {
                warnings_found += check_unused_variables_with_usage(node->data.call.args, table, usage_list);
            }
            break;
        }
        
        case AST_BINOP:
        case AST_UNARYOP: {
            if (node->type == AST_BINOP) {
                warnings_found += check_unused_variables_with_usage(node->data.binop.left, table, usage_list);
                warnings_found += check_unused_variables_with_usage(node->data.binop.right, table, usage_list);
            } else {
                warnings_found += check_unused_variables_with_usage(node->data.unaryop.expr, table, usage_list);
            }
            break;
        }
        
        case AST_IF: {
            warnings_found += check_unused_variables_with_usage(node->data.if_stmt.condition, table, usage_list);
            warnings_found += check_unused_variables_with_usage(node->data.if_stmt.then_body, table, usage_list);
            if (node->data.if_stmt.else_body) {
                warnings_found += check_unused_variables_with_usage(node->data.if_stmt.else_body, table, usage_list);
            }
            break;
        }
        
        case AST_WHILE: {
            warnings_found += check_unused_variables_with_usage(node->data.while_stmt.condition, table, usage_list);
            warnings_found += check_unused_variables_with_usage(node->data.while_stmt.body, table, usage_list);
            break;
        }
        
        case AST_FOR: {
            warnings_found += check_unused_variables_with_usage(node->data.for_stmt.start, table, usage_list);
            warnings_found += check_unused_variables_with_usage(node->data.for_stmt.end, table, usage_list);
            if (node->data.for_stmt.var && node->data.for_stmt.var->type == AST_IDENTIFIER) {//获取循环变量定义的行号和列号
                int line = (node->data.for_stmt.var->location.first_line > 0) ? node->data.for_stmt.var->location.first_line : 1;
                int column = (node->data.for_stmt.var->location.first_column > 0) ? node->data.for_stmt.var->location.first_column : 1;
                add_symbol(table, node->data.for_stmt.var->data.identifier.name, SYMBOL_VARIABLE, TYPE_UNKNOWN);
                VariableUsage* var_usage = find_variable_in_usage(*usage_list, node->data.for_stmt.var->data.identifier.name);
                if (!var_usage) {
                    VariableUsage* new_usage = add_variable_to_usage_with_column(*usage_list, node->data.for_stmt.var->data.identifier.name, line, column);
                    *usage_list = new_usage;
                }
            }
            warnings_found += check_unused_variables_with_usage(node->data.for_stmt.body, table, usage_list);
            break;
        }
        
        case AST_PRINT: {
            warnings_found += check_unused_variables_with_usage(node->data.print.expr, table, usage_list);
            break;
        }
        
        case AST_INPUT: {
            if (node->data.input.prompt) {
                warnings_found += check_unused_variables_with_usage(node->data.input.prompt, table, usage_list);
            }
            break;
        }
        
        case AST_TOINT:
        case AST_TOFLOAT: {
            if (node->type == AST_TOINT) {
                warnings_found += check_unused_variables_with_usage(node->data.toint.expr, table, usage_list);
            } else {
                warnings_found += check_unused_variables_with_usage(node->data.tofloat.expr, table, usage_list);
            }
            break;
        }
        
        case AST_RETURN: {
            if (node->data.return_stmt.expr) {
                warnings_found += check_unused_variables_with_usage(node->data.return_stmt.expr, table, usage_list);
            }
            break;
        }
        
        case AST_EXPRESSION_LIST: {
            for (int i = 0; i < node->data.expression_list.expression_count; i++) {
                warnings_found += check_unused_variables_with_usage(node->data.expression_list.expressions[i], table, usage_list);
            }
            break;
        }

        case AST_STRUCT_LITERAL: {
            ASTNode* fields = node->data.struct_literal.fields;
            if (fields && fields->type == AST_EXPRESSION_LIST) {
                for (int i = 0; i < fields->data.expression_list.expression_count; i++) {
                    ASTNode* field = fields->data.expression_list.expressions[i];
                    if (field && field->type == AST_ASSIGN) {
                        warnings_found += check_unused_variables_with_usage(field->data.assign.right, table, usage_list);
                    } else {
                        warnings_found += check_unused_variables_with_usage(field, table, usage_list);
                    }
                }
            }
            break;
        }

        case AST_INDEX: {
            if (node->data.index.target) warnings_found += check_unused_variables_with_usage(node->data.index.target, table, usage_list);
            if (node->data.index.index && node->data.index.index->type != AST_IDENTIFIER) {
                if (node->data.index.index) warnings_found += check_unused_variables_with_usage(node->data.index.index, table, usage_list);
            }
            break;
        }
        
        case AST_MEMBER_ACCESS: {
            if (node->data.member_access.object) warnings_found += check_unused_variables_with_usage(node->data.member_access.object, table, usage_list);
            if (node->data.member_access.field && node->data.member_access.field->type != AST_IDENTIFIER) {
                if (node->data.member_access.field) warnings_found += check_unused_variables_with_usage(node->data.member_access.field, table, usage_list);
            }
            break;
        }
        
        case AST_NUM_INT:
        case AST_NUM_FLOAT:
        case AST_STRING:
            break;
            
        default:
            break;
    }
    
    return warnings_found;
}

int check_undefined_symbols_in_node(ASTNode* node, SymbolTable* table) {
    return check_undefined_symbols_in_node_with_visited(node, table, NULL);
}
static int extract_public_functions_from_module_recurse(const char* module_path, SymbolTable* table, VixVisitedModule* visited) {
    VixVisitedModule* v = visited;
    while (v) {
        if (v->path && strcmp(v->path, module_path) == 0) {
            return 0;
        }
        v = v->next;
    }

    FILE* file = fopen(module_path, "r");
    if (!file) {
        return 0;
    }
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* buffer = malloc(file_size + 1);
    if (!buffer) {
        fclose(file);
        return 0;
    }
    
    fread(buffer, 1, file_size, file);
    buffer[file_size] = '\0';
    fclose(file);
    
    VixVisitedModule current_visited;
    current_visited.path = module_path;
    current_visited.next = visited;
    
    int errors_found = 0;
    char* pos = buffer;
    while ((pos = strstr(pos, "pub fn")) != NULL) {
        pos += 6;
        while (*pos && isspace(*pos)) {
            pos++;
        }
        char func_name[256];
        int i = 0;
        if ((*pos >= 'a' && *pos <= 'z') || (*pos >= 'A' && *pos <= 'Z') || *pos == '_') {
            func_name[i++] = *pos++;
            while ((*pos >= 'a' && *pos <= 'z') || 
                   (*pos >= 'A' && *pos <= 'Z') || 
                   (*pos >= '0' && *pos <= '9') || 
                   *pos == '_') {
                if (i < (int)(sizeof(func_name) - 1)) {
                    func_name[i++] = *pos;
                }
                pos++;
            }
        }
        if (i > 0) {
            func_name[i] = '\0';
            add_symbol(table, func_name, SYMBOL_FUNCTION, TYPE_UNKNOWN);
        }
    }
    
    pos = buffer;
    while ((pos = strstr(pos, "import \"")) != NULL) {
        pos += 8;
        char import_path[1024];
        int i = 0;
        while (*pos && *pos != '"' && i < (int)(sizeof(import_path) - 1)) {
            import_path[i++] = *pos++;
        }
        import_path[i] = '\0';
        if (*pos == '"') {
            pos++;
        }
        if (i > 0) {
            char resolved[1024];
            if (vix_resolve_import_path(module_path, import_path, resolved, sizeof(resolved))) {
                errors_found += extract_public_functions_from_module_recurse(resolved, table, &current_visited);
            }
        }
    }
    
    free(buffer);
    return errors_found;
}

static int extract_public_functions_from_module(const char* module_path, SymbolTable* table) {
    return extract_public_functions_from_module_recurse(module_path, table, NULL);
}
