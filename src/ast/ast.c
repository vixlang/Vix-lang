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

#include "../include/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/compat.h"

#ifdef HAVE_PARSER_TAB_H//tips : 别删，用来取消警告
#include "../parser/parser.tab.h"//tips : 这个头文件按编译顺序编译
#endif
extern FILE* yyin;
extern ASTNode* root; //tips : 根节点
extern const char* current_input_filename;
extern int yyparse(void);
extern int yylineno;
extern int yycolumn;

typedef struct yy_buffer_state* YY_BUFFER_STATE;
extern YY_BUFFER_STATE yy_create_buffer(FILE* file, int size);
extern void yypush_buffer_state(YY_BUFFER_STATE new_buffer);
extern void yypop_buffer_state(void);

#ifndef VIX_FLEX_BUFFER_SIZE
#define VIX_FLEX_BUFFER_SIZE 16384
#endif

typedef struct ImportedModuleNode {
    char* canonical_path;
    struct ImportedModuleNode* next;
} ImportedModuleNode;

static ImportedModuleNode* g_imported_module_cache = NULL;

static ASTNode* alloc_ast_node(void) {
    ASTNode* node = (ASTNode*)calloc(1, sizeof(ASTNode));
    if (!node) {
        fprintf(stderr, "Failed to allocate memory for ASTNode\n");
        exit(1);
    }
    return node;
}

void free_type_info(TypeInfo* info) {
    if (!info) return;

    if (info->element) {
        free_type_info(info->element);
    }
    if (info->name) {
        free(info->name);
    }
    if (info->params) {
        for (int i = 0; i < info->param_count; i++) {
            free_type_info(info->params[i]);
        }
        free(info->params);
    }
    if (info->ret) {
        free_type_info(info->ret);
    }
    if (info->app_ctor) {
        free_type_info(info->app_ctor);
    }
    if (info->app_args) {
        for (int i = 0; i < info->app_arg_count; i++) {
            free_type_info(info->app_args[i]);
        }
        free(info->app_args);
    }
    free(info);
}

static int canonicalize_existing_path(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) return 0;

    char resolved[1024];
    if (vix_realpath(path, resolved, sizeof(resolved)) != NULL) {
        strncpy(out, resolved, out_size - 1);
        out[out_size - 1] = '\0';
        return 1;
    }

    strncpy(out, path, out_size - 1);
    out[out_size - 1] = '\0';
    return 1;
}

static int vix_expand_package_name(const char* module_path, char* out, size_t out_size) {
    if (!module_path || !out || out_size == 0) return 0;
    if (strchr(module_path, '/') != NULL) return 0;

    char registry[256] = "github.com";
    char user[256] = "";
    char repo[256] = "";
    const char* p = module_path;

    if (p[0] == '@') {
        strncpy(registry, "gitee.com", sizeof(registry) - 1);
        registry[sizeof(registry) - 1] = '\0';
        p++;
    } else {
        const char* colon = strchr(p, ':');
        if (colon) {
            size_t reg_len = colon - p;
            if (reg_len > 0 && reg_len < sizeof(registry)) {
                strncpy(registry, p, reg_len);
                registry[reg_len] = '\0';
                if (strchr(registry, '.') == NULL) {
                    size_t avail = sizeof(registry) - strlen(registry) - 1;
                    strncat(registry, ".com", avail);
                }
            }
            p = colon + 1;
        }
    }

    const char* dot = strchr(p, '.');
    if (dot) {
        size_t user_len = dot - p;
        if (user_len > 0 && user_len < sizeof(user)) {
            strncpy(user, p, user_len);
            user[user_len] = '\0';
            strncpy(repo, dot + 1, sizeof(repo) - 1);
            repo[sizeof(repo) - 1] = '\0';
        } else {
            return 0;
        }
    } else {
        strncpy(user, "vixlang", sizeof(user) - 1);
        user[sizeof(user) - 1] = '\0';
        int n = snprintf(repo, sizeof(repo), "vlib-%s", p);
        if (n < 0 || (size_t)n >= sizeof(repo)) return 0;
    }

    int n = snprintf(out, out_size, "%s/%s/%s", registry, user, repo);
    if (n < 0 || (size_t)n >= out_size) return 0;
    return 1;
}

static int try_resolve_import_path(const char* base_dir, const char* module_path, char* out, size_t out_size) {
    char candidate[1024];
    snprintf(candidate, sizeof(candidate), "%s/%s", base_dir, module_path);
    if (vix_file_exists(candidate)) {
        return canonicalize_existing_path(candidate, out, out_size);
    }
    return 0;
}

int vix_resolve_import_path(const char* current_file, const char* module_path, char* out, size_t out_size) {
    if (!module_path || !out || out_size == 0) return 0;

    // Priority 1: relative to current file's directory (existing behavior)
    if (module_path[0] == '/') {
        if (vix_file_exists(module_path)) {
            return canonicalize_existing_path(module_path, out, out_size);
        }
    } else if (current_file) {
        char dir_path[1024];
        strncpy(dir_path, current_file, sizeof(dir_path) - 1);
        dir_path[sizeof(dir_path) - 1] = '\0';
        char* last_slash = strrchr(dir_path, '/');
        if (last_slash) {
            *(last_slash + 1) = '\0';
        } else {
            strcpy(dir_path, "./");
        }
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s%s", dir_path, module_path);
        if (vix_file_exists(candidate)) {
            return canonicalize_existing_path(candidate, out, out_size);
        }
    }

    // Priority 2: package name expansion (for bare names without '/')
    if (strchr(module_path, '/') == NULL) {
        char expanded[1024];
        if (vix_expand_package_name(module_path, expanded, sizeof(expanded))) {
            const char* vix_home = vix_getenv("VIX_HOME");
            char lib_path[1024];

            snprintf(lib_path, sizeof(lib_path), ".vix/libs/%s/main.vix", expanded);
            if (vix_file_exists(lib_path)) {
                return canonicalize_existing_path(lib_path, out, out_size);
            }

            if (vix_home && vix_home[0] != '\0') {
                snprintf(lib_path, sizeof(lib_path), "%s/libs/%s/main.vix", vix_home, expanded);
                if (vix_file_exists(lib_path)) {
                    return canonicalize_existing_path(lib_path, out, out_size);
                }

                // Bare name standard library fallback
                snprintf(lib_path, sizeof(lib_path), "%s/std/%s", vix_home, module_path);
                if (vix_file_exists(lib_path)) {
                    return canonicalize_existing_path(lib_path, out, out_size);
                }
            }
        }
    } else {
        // Priority 3: path-style import (contains '/'), backward-compatible direct search
        const char* vix_home = vix_getenv("VIX_HOME");
        if (vix_home && vix_home[0] != '\0') {
            char base[1024];
            snprintf(base, sizeof(base), "%s/std", vix_home);
            if (try_resolve_import_path(base, module_path, out, out_size)) return 1;
        }
        if (try_resolve_import_path(".vix/libs", module_path, out, out_size)) return 1;
        if (vix_home && vix_home[0] != '\0') {
            char base[1024];
            snprintf(base, sizeof(base), "%s/libs", vix_home);
            if (try_resolve_import_path(base, module_path, out, out_size)) return 1;
        }
    }

    return 0;
}

// Deprecated: use vix_resolve_import_path instead
static int resolve_import_path(const char* current_file, const char* module_path, char* out, size_t out_size) {
    return vix_resolve_import_path(current_file, module_path, out, out_size);
}

static int import_cache_contains(const char* canonical_path) {
    if (!canonical_path) return 0;
    ImportedModuleNode* cur = g_imported_module_cache;
    while (cur) {
        if (cur->canonical_path && strcmp(cur->canonical_path, canonical_path) == 0) {
            return 1;
        }
        cur = cur->next;
    }
    return 0;
}

static void import_cache_add(const char* canonical_path) {
    if (!canonical_path || import_cache_contains(canonical_path)) return;

    ImportedModuleNode* node = malloc(sizeof(ImportedModuleNode));
    if (!node) return;
    node->canonical_path = strdup(canonical_path);
    if (!node->canonical_path) {
        free(node);
        return;
    }
    node->next = g_imported_module_cache;
    g_imported_module_cache = node;
}

static void clear_import_cache(void) {
    ImportedModuleNode* cur = g_imported_module_cache;
    while (cur) {
        ImportedModuleNode* next = cur->next;
        free(cur->canonical_path);
        free(cur);
        cur = next;
    }
    g_imported_module_cache = NULL;
}

typedef struct ParserImportState {
    FILE* yyin;
    ASTNode* root;
    const char* current_input_filename;
    int yylineno;
    int yycolumn;
} ParserImportState;

static ASTNode* parse_imported_module_ast(const char* full_module_path) {
    if (!full_module_path) return NULL;

    FILE* f = fopen(full_module_path, "r");
    if (!f) return NULL;

    ParserImportState old_state = {
        .yyin = yyin,
        .root = root,
        .current_input_filename = current_input_filename,
        .yylineno = yylineno,
        .yycolumn = yycolumn
    };

    ASTNode* module_root = NULL;
    YY_BUFFER_STATE import_buffer = yy_create_buffer(f, VIX_FLEX_BUFFER_SIZE);
    if (!import_buffer) {
        fclose(f);
        return NULL;
    }

    yyin = f;
    yypush_buffer_state(import_buffer);
    current_input_filename = full_module_path;
    root = NULL;
    yylineno = 1;
    yycolumn = 1;

    int parse_result = yyparse();
    module_root = root;

    yypop_buffer_state();
    fclose(f);

    yyin = old_state.yyin;
    root = old_state.root;
    current_input_filename = old_state.current_input_filename;
    yylineno = old_state.yylineno;
    yycolumn = old_state.yycolumn;

    if (parse_result != 0) {
        if (module_root) free_ast(module_root);
        return NULL;
    }

    return module_root;
}

static void remove_program_statement_at(ASTNode* program, int idx) {
    if (!program || program->type != AST_PROGRAM) return;
    if (idx < 0 || idx >= program->data.program.statement_count) return;

    free_ast(program->data.program.statements[idx]);

    for (int k = idx + 1; k < program->data.program.statement_count; k++) {
        program->data.program.statements[k - 1] = program->data.program.statements[k];
    }

    program->data.program.statement_count--;
    if (program->data.program.statement_count == 0) {
        free(program->data.program.statements);
        program->data.program.statements = NULL;
    } else {
        ASTNode** resized = realloc(program->data.program.statements,
                                    sizeof(ASTNode*) * program->data.program.statement_count);
        if (resized) {
            program->data.program.statements = resized;
        }
    }
}

static void set_source_file_recursive(ASTNode* node, const char* source_file) {
    if (!node) return;
    node->source_file = source_file;

    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->data.program.statement_count; i++) {
                set_source_file_recursive(node->data.program.statements[i], source_file);
            }
            break;
        case AST_PRINT:
            set_source_file_recursive(node->data.print.expr, source_file);
            break;
        case AST_INPUT:
            set_source_file_recursive(node->data.input.prompt, source_file);
            break;
        case AST_TOINT:
            set_source_file_recursive(node->data.toint.expr, source_file);
            break;
        case AST_TOFLOAT:
            set_source_file_recursive(node->data.tofloat.expr, source_file);
            break;
        case AST_EXPRESSION_LIST:
            for (int i = 0; i < node->data.expression_list.expression_count; i++) {
                set_source_file_recursive(node->data.expression_list.expressions[i], source_file);
            }
            break;
        case AST_ASSIGN:
        case AST_CONST:
            set_source_file_recursive(node->data.assign.left, source_file);
            set_source_file_recursive(node->data.assign.right, source_file);
            break;
        case AST_BINOP:
            set_source_file_recursive(node->data.binop.left, source_file);
            set_source_file_recursive(node->data.binop.right, source_file);
            break;
        case AST_UNARYOP:
            set_source_file_recursive(node->data.unaryop.expr, source_file);
            break;
        case AST_TYPE_LIST:
            set_source_file_recursive(node->data.list_type.element_type, source_file);
            break;
        case AST_TYPE_FIXED_SIZE_LIST:
            set_source_file_recursive(node->data.fixed_size_list_type.element_type, source_file);
            break;
        case AST_IF:
            set_source_file_recursive(node->data.if_stmt.condition, source_file);
            set_source_file_recursive(node->data.if_stmt.then_body, source_file);
            set_source_file_recursive(node->data.if_stmt.else_body, source_file);
            break;
        case AST_WHILE:
            set_source_file_recursive(node->data.while_stmt.condition, source_file);
            set_source_file_recursive(node->data.while_stmt.body, source_file);
            break;
        case AST_FOR:
            set_source_file_recursive(node->data.for_stmt.var, source_file);
            set_source_file_recursive(node->data.for_stmt.start, source_file);
            set_source_file_recursive(node->data.for_stmt.end, source_file);
            set_source_file_recursive(node->data.for_stmt.body, source_file);
            break;
        case AST_INDEX:
            set_source_file_recursive(node->data.index.target, source_file);
            set_source_file_recursive(node->data.index.index, source_file);
            break;
        case AST_MEMBER_ACCESS:
            set_source_file_recursive(node->data.member_access.object, source_file);
            set_source_file_recursive(node->data.member_access.field, source_file);
            break;
        case AST_FUNCTION:
            set_source_file_recursive(node->data.function.params, source_file);
            set_source_file_recursive(node->data.function.generic_params, source_file);
            set_source_file_recursive(node->data.function.return_type, source_file);
            set_source_file_recursive(node->data.function.body, source_file);
            break;
        case AST_CALL:
            set_source_file_recursive(node->data.call.func, source_file);
            set_source_file_recursive(node->data.call.args, source_file);
            set_source_file_recursive(node->data.call.type_args, source_file);
            break;
        case AST_STRUCT_DEF:
            set_source_file_recursive(node->data.struct_def.fields, source_file);
            set_source_file_recursive(node->data.struct_def.generic_params, source_file);
            break;
        case AST_STRUCT_LITERAL:
            set_source_file_recursive(node->data.struct_literal.type_name, source_file);
            set_source_file_recursive(node->data.struct_literal.fields, source_file);
            break;
        case AST_RETURN:
            set_source_file_recursive(node->data.return_stmt.expr, source_file);
            break;
        case AST_GLOBAL:
            set_source_file_recursive(node->data.global_decl.identifier, source_file);
            set_source_file_recursive(node->data.global_decl.type, source_file);
            set_source_file_recursive(node->data.global_decl.initializer, source_file);
            break;
        default:
            break;
    }
}

ASTNode* create_program_node_with_location(Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_PROGRAM;
    node->location = location;
    node->data.program.statements = NULL;
    node->data.program.statement_count = 0;
    return node;
}

ASTNode* create_program_node() {
    Location loc = {1, 1, 1, 1};
    return create_program_node_with_location(loc);
}

void add_statement_to_program(ASTNode* program, ASTNode* statement) {
    if (!program || program->type != AST_PROGRAM || !statement) return;

    if (statement->type == AST_PROGRAM) {
        int nested_count = statement->data.program.statement_count;
        for (int i = 0; i < nested_count; i++) {
            add_statement_to_program(program, statement->data.program.statements[i]);
        }
        return;
    }

    program->data.program.statement_count++;
    program->data.program.statements = realloc(
        program->data.program.statements,
        sizeof(ASTNode*) * program->data.program.statement_count
    );
    program->data.program.statements[program->data.program.statement_count - 1] = statement;
}

ASTNode* create_print_node_with_location(ASTNode* expr, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_PRINT;
    node->location = location;
    node->data.print.expr = expr;
    return node;
}

ASTNode* create_print_node(ASTNode* expr) {
    return create_print_node_with_location(expr, expr->location);
}

ASTNode* create_input_node_with_location(ASTNode* prompt, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_INPUT;
    node->location = location;
    node->data.input.prompt = prompt;
    return node;
}

ASTNode* create_input_node(ASTNode* prompt) {
    return create_input_node_with_location(prompt, prompt->location);
}

ASTNode* create_input_node_with_yyltype(ASTNode* prompt, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_input_node_with_location(prompt, location);
}

ASTNode* create_toint_node_with_location(ASTNode* expr, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_TOINT;
    node->location = location;
    node->data.toint.expr = expr;
    return node;
}

ASTNode* create_toint_node(ASTNode* expr) {
    return create_toint_node_with_location(expr, expr->location);
}

ASTNode* create_tofloat_node_with_location(ASTNode* expr, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_TOFLOAT;
    node->location = location;
    node->data.tofloat.expr = expr;
    return node;
}

ASTNode* create_tofloat_node(ASTNode* expr) {
    return create_tofloat_node_with_location(expr, expr->location);
}

ASTNode* create_nil_node_with_location(Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_NIL;
    node->location = location;
    return node;
}

ASTNode* create_nil_node() {
    Location loc = {1, 1, 1, 1};
    return create_nil_node_with_location(loc);
}

ASTNode* create_nil_node_with_yyltype(void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_nil_node_with_location(location);
}

ASTNode* create_expression_list_node_with_location(Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_EXPRESSION_LIST;
    node->location = location;
    node->data.expression_list.expressions = NULL;
    node->data.expression_list.expression_count = 0;
    node->data.expression_list.precomputed_length = -1;  // -1表示未计算
    return node;
}

ASTNode* create_expression_list_node() {
    Location loc = {1, 1, 1, 1};
    return create_expression_list_node_with_location(loc);
}

void add_expression_to_list(ASTNode* list, ASTNode* expr) {
    if (!list || list->type != AST_EXPRESSION_LIST || !expr) return;
    
    list->data.expression_list.expression_count++;
    list->data.expression_list.expressions = realloc(
        list->data.expression_list.expressions,
        sizeof(ASTNode*) * list->data.expression_list.expression_count
    );
    list->data.expression_list.expressions[list->data.expression_list.expression_count - 1] = expr;
}

ASTNode* create_assign_node_with_location(ASTNode* left, ASTNode* right, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_ASSIGN;
    node->location = location;
    node->data.assign.left = left;
    node->data.assign.right = right;
    node->data.assign.is_declaration = 0;
    node->data.assign.is_public = 0;
    return node;
}

ASTNode* create_const_node_with_location(ASTNode* left, ASTNode* right, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_CONST;
    node->location = location;
    node->data.assign.left = left;
    node->data.assign.right = right;
    node->data.assign.is_declaration = 1;
    node->data.assign.is_public = 0;
    return node;
}

ASTNode* create_const_node(ASTNode* left, ASTNode* right) {
    Location loc = {
        left->location.first_line,
        left->location.first_column,
        right->location.last_line,
        right->location.last_column
    };
    return create_const_node_with_location(left, right, loc);
}

ASTNode* create_assign_node(ASTNode* left, ASTNode* right) {
    Location loc = {
        left->location.first_line,
        left->location.first_column,
        right->location.last_line,
        right->location.last_column
    };
    return create_assign_node_with_location(left, right, loc);
}

ASTNode* create_assign_node_with_yyltype(ASTNode* left, ASTNode* right, void* yylloc) {
    ASTNode* node = alloc_ast_node();
    
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    node->location.first_line = loc->first_line;
    node->location.first_column = loc->first_column;
    node->location.last_line = loc->last_line;
    node->location.last_column = loc->last_column;
    node->type = AST_ASSIGN;
    node->data.assign.left = left;
    node->data.assign.right = right;
    node->data.assign.is_declaration = 0;
    node->data.assign.is_public = 0;
    node->mutability = MUTABILITY_IMMUTABLE;//默认为不可变
    
    return node;
}

ASTNode* create_assign_node_with_mutability(ASTNode* left, ASTNode* right, MutabilityType mutability) {
    ASTNode* node = alloc_ast_node();
    
    node->type = AST_ASSIGN;
    node->data.assign.left = left;
    node->data.assign.right = right;
    node->data.assign.is_declaration = 0;
    node->data.assign.is_public = 0;
    node->mutability = mutability;
    
    return node;
}

ASTNode* create_binop_node_with_location(BinOpType op, ASTNode* left, ASTNode* right, Location location) {
    if (left->type == AST_STRING && right->type == AST_STRING) {// 尝试进行常量折叠优化
        if (op == OP_ADD || op == OP_CONCAT) {//尝试进行字符串拼接
            size_t len1 = strlen(left->data.string.value);
            size_t len2 = strlen(right->data.string.value);
            char* result = malloc(len1 + len2 + 1);
            strcpy(result, left->data.string.value);
            strcat(result, right->data.string.value);
            ASTNode* new_node = create_string_node_with_location(result, location);// 创建新的字符串节点并释放临时节点
            free(result);
            free_ast(left);
            free_ast(right);
            return new_node;
        }
        else if (op == OP_MUL || op == OP_REPEAT) {
            //右操作数不是数字，故不做任何操作
        }
    }
    else if (left->type == AST_STRING && (right->type == AST_NUM_INT || right->type == AST_NUM_FLOAT)) {
        if (op == OP_MUL || op == OP_REPEAT) {
            char* str_val = left->data.string.value;
            int repeat_times;
            if (right->type == AST_NUM_INT) {
                repeat_times = (int)right->data.num_int.value;
            } else {
                repeat_times = (int)right->data.num_float.value;
            }
            
            if (repeat_times < 0) repeat_times = 0;
            
            size_t str_len = strlen(str_val);
            size_t total_len = str_len * repeat_times;
            char* result = malloc(total_len + 1);
            result[0] = '\0';
            
            for (int i = 0; i < repeat_times; i++) {
                strcat(result, str_val);
            }
            
            // 创建新的字符串节点并释放临时节点
            ASTNode* new_node = create_string_node_with_location(result, location);
            free(result);
            free_ast(left);
            free_ast(right);
            return new_node;
        }
    }
    else if (left->type == AST_NUM_INT && right->type == AST_NUM_INT) {
        switch(op) {
            case OP_ADD: {
                ASTNode* new_node = create_num_int_node_with_location(
                    left->data.num_int.value + right->data.num_int.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_SUB: {
                ASTNode* new_node = create_num_int_node_with_location(
                    left->data.num_int.value - right->data.num_int.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_MUL: {
                ASTNode* new_node = create_num_int_node_with_location(
                    left->data.num_int.value * right->data.num_int.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_DIV: {
                if (right->data.num_int.value != 0) {
                    ASTNode* new_node = create_num_int_node_with_location(
                        left->data.num_int.value / right->data.num_int.value, 
                        location
                    );
                    free_ast(left);
                    free_ast(right);
                    return new_node;
                }
                break;
            }
            case OP_MOD: {
                if (right->data.num_int.value != 0) {
                    ASTNode* new_node = create_num_int_node_with_location(
                        left->data.num_int.value % right->data.num_int.value, 
                        location
                    );
                    free_ast(left);
                    free_ast(right);
                    return new_node;
                }
                break;
            }
            default:
                break;
        }
    }
    else if (left->type == AST_NUM_FLOAT && right->type == AST_NUM_FLOAT) {
        switch(op) {
            case OP_ADD: {
                ASTNode* new_node = create_num_float_node_with_location(
                    left->data.num_float.value + right->data.num_float.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_SUB: {
                ASTNode* new_node = create_num_float_node_with_location(
                    left->data.num_float.value - right->data.num_float.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_MUL: {
                ASTNode* new_node = create_num_float_node_with_location(
                    left->data.num_float.value * right->data.num_float.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_DIV: {
                if (right->data.num_float.value != 0.0) {
                    ASTNode* new_node = create_num_float_node_with_location(
                        left->data.num_float.value / right->data.num_float.value, 
                        location
                    );
                    free_ast(left);
                    free_ast(right);
                    return new_node;
                }
                break;
            }
            default:
                break;
        }
    }
    // 混合整数和浮点数
    else if (left->type == AST_NUM_INT && right->type == AST_NUM_FLOAT) {
        switch(op) {
            case OP_ADD: {
                ASTNode* new_node = create_num_float_node_with_location(
                    left->data.num_int.value + right->data.num_float.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_SUB: {
                ASTNode* new_node = create_num_float_node_with_location(
                    left->data.num_int.value - right->data.num_float.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_MUL: {
                ASTNode* new_node = create_num_float_node_with_location(
                    left->data.num_int.value * right->data.num_float.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_DIV: {
                if (right->data.num_float.value != 0.0) {
                    ASTNode* new_node = create_num_float_node_with_location(
                        left->data.num_int.value / right->data.num_float.value, 
                        location
                    );
                    free_ast(left);
                    free_ast(right);
                    return new_node;
                }
                break;
            }
            default:
                break;
        }
    }
    else if (left->type == AST_NUM_FLOAT && right->type == AST_NUM_INT) {
        switch(op) {
            case OP_ADD: {
                ASTNode* new_node = create_num_float_node_with_location(
                    left->data.num_float.value + right->data.num_int.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_SUB: {
                ASTNode* new_node = create_num_float_node_with_location(
                    left->data.num_float.value - right->data.num_int.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_MUL: {
                ASTNode* new_node = create_num_float_node_with_location(
                    left->data.num_float.value * right->data.num_int.value, 
                    location
                );
                free_ast(left);
                free_ast(right);
                return new_node;
            }
            case OP_DIV: {
                if (right->data.num_int.value != 0) {
                    ASTNode* new_node = create_num_float_node_with_location(
                        left->data.num_float.value / right->data.num_int.value, 
                        location
                    );
                    free_ast(left);
                    free_ast(right);
                    return new_node;
                }
                break;
            }
            default:
                break;
        }
    }
    ASTNode* node = alloc_ast_node();// 如果不能折叠，则创建正常的二元操作节点
    node->type = AST_BINOP;
    node->location = location;
    node->data.binop.op = op;
    node->data.binop.left = left;
    node->data.binop.right = right;
    return node;
}

ASTNode* create_binop_node(BinOpType op, ASTNode* left, ASTNode* right) {
    Location loc = {
        left->location.first_line,
        left->location.first_column,
        right->location.last_line,
        right->location.last_column
    };
    return create_binop_node_with_location(op, left, right, loc);
}

ASTNode* create_unaryop_node_with_location(UnaryOpType op, ASTNode* expr, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_UNARYOP;
    node->location = location;
    node->data.unaryop.op = op;
    node->data.unaryop.expr = expr;
    return node;
}

ASTNode* create_unaryop_node_with_yyltype(UnaryOpType op, ASTNode* expr, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_unaryop_node_with_location(op, expr, location);
}

ASTNode* create_unaryop_node(UnaryOpType op, ASTNode* expr) {
    Location loc = {
        expr->location.first_line,
        expr->location.first_column,
        expr->location.last_line,
        expr->location.last_column
    };
    return create_unaryop_node_with_location(op, expr, loc);
}

ASTNode* create_num_int_node_with_location(long long value, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_NUM_INT;
    node->location = location;
    node->data.num_int.value = value;
    return node;
}

ASTNode* create_num_int_node(long long value) {
    Location loc = {1, 1, 1, 1};
    return create_num_int_node_with_location(value, loc);
}

ASTNode* create_num_float_node_with_location(double value, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_NUM_FLOAT;
    node->location = location;
    node->data.num_float.value = value;
    return node;
}

ASTNode* create_num_float_node(double value) {
    Location loc = {1, 1, 1, 1};
    return create_num_float_node_with_location(value, loc);
}

ASTNode* create_string_node_with_location(const char* value, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_STRING;
    node->location = location;
    node->data.string.value = malloc(strlen(value) + 1);
    strcpy(node->data.string.value, value);
    return node;
}

ASTNode* create_string_node(const char* value) {
    Location loc = {1, 1, 1, 1};
    return create_string_node_with_location(value, loc);
}

ASTNode* create_identifier_node_with_location(const char* name, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_IDENTIFIER;
    node->location = location;
    node->data.identifier.name = malloc(strlen(name) + 1);
    strcpy(node->data.identifier.name, name);
    return node;
}

ASTNode* create_identifier_node(const char* name) {
    Location loc = {1, 1, 1, 1};
    return create_identifier_node_with_location(name, loc);
}

ASTNode* create_type_node_with_location(NodeType type, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = type;
    node->location = location;
    return node;
}

ASTNode* create_type_node(NodeType type) {
    ASTNode* node = alloc_ast_node();
    Location loc = {0};
    node->type = type;
    node->location = loc;
    return node;
}
ASTNode* create_list_type_node_with_location(ASTNode* element_type, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_TYPE_LIST;
    node->location = location;
    node->data.list_type.element_type = element_type;
    return node;
}

ASTNode* create_list_type_node(ASTNode* element_type) {
    Location loc = {0};
    return create_list_type_node_with_location(element_type, loc);
}
ASTNode* create_fixed_size_list_type_node_with_location(ASTNode* element_type, long long size, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_TYPE_FIXED_SIZE_LIST;
    node->location = location;
    node->data.fixed_size_list_type.element_type = element_type;
    node->data.fixed_size_list_type.size = size;
    return node;
}

ASTNode* create_fixed_size_list_type_node(ASTNode* element_type, long long size) {
    Location loc = {0};
    return create_fixed_size_list_type_node_with_location(element_type, size, loc);
}

ASTNode* create_if_node_with_location(ASTNode* condition, ASTNode* then_body, ASTNode* else_body, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_IF;
    node->location = location;
    node->data.if_stmt.condition = condition;
    node->data.if_stmt.then_body = then_body;
    node->data.if_stmt.else_body = else_body;
    return node;
}

ASTNode* create_if_node(ASTNode* condition, ASTNode* then_body, ASTNode* else_body) {
    Location loc = {
        condition->location.first_line,
        condition->location.first_column,
        else_body ? else_body->location.last_line : then_body->location.last_line,
        else_body ? else_body->location.last_column : then_body->location.last_column
    };
    return create_if_node_with_location(condition, then_body, else_body, loc);
}

ASTNode* create_while_node_with_location(ASTNode* condition, ASTNode* body, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_WHILE;
    node->location = location;
    node->data.while_stmt.condition = condition;
    node->data.while_stmt.body = body;
    return node;
}

ASTNode* create_while_node(ASTNode* condition, ASTNode* body) {
    Location loc = {
        condition->location.first_line,
        condition->location.first_column,
        body->location.last_line,
        body->location.last_column
    };
    return create_while_node_with_location(condition, body, loc);
}

ASTNode* create_for_node_with_location(ASTNode* var, ASTNode* start, ASTNode* end, ASTNode* body, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_FOR;
    node->location = location;
    node->data.for_stmt.var = var;
    node->data.for_stmt.start = start;
    node->data.for_stmt.end = end;
    node->data.for_stmt.body = body;
    return node;
}

ASTNode* create_for_node(ASTNode* var, ASTNode* start, ASTNode* end, ASTNode* body) {
    Location loc = {
        var->location.first_line,
        var->location.first_column,
        body->location.last_line,
        body->location.last_column
    };
    return create_for_node_with_location(var, start, end, body, loc);
}

ASTNode* create_function_node_with_location(const char* name, ASTNode* params, ASTNode* return_type, ASTNode* body, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_FUNCTION;
    node->location = location;
    node->data.function.name = malloc(strlen(name) + 1);
    strcpy(node->data.function.name, name);
    node->data.function.params = params;
    node->data.function.generic_params = NULL;
    node->data.function.return_type = return_type;
    node->data.function.body = body;
    node->data.function.is_extern = 0;
    node->data.function.linkage = NULL;
    node->data.function.vararg = 0;
    node->data.function.is_public = 0;  // 默认不是公共函数
    return node;
}

ASTNode* create_function_node(const char* name, ASTNode* params, ASTNode* return_type, ASTNode* body) {
    Location loc = {0, 0, 0, 0};
    if (body) {
        loc.first_line = 0;
        loc.first_column = 0;
        loc.last_line = body->location.last_line;
        loc.last_column = body->location.last_column;
    }
    return create_function_node_with_location(name, params, return_type, body, loc);
}

ASTNode* create_public_function_node(const char* name, ASTNode* params, ASTNode* return_type, ASTNode* body) {
    Location loc = {0, 0, 0, 0};
    if (body) {
        loc.first_line = 0;
        loc.first_column = 0;
        loc.last_line = body->location.last_line;
        loc.last_column = body->location.last_column;
    }
    ASTNode* node = create_function_node_with_location(name, params, return_type, body, loc);
    node->data.function.is_public = 1;//pub
    return node;
}

ASTNode* create_public_function_node_with_location(const char* name, ASTNode* params, ASTNode* return_type, ASTNode* body, Location location) {
    ASTNode* node = create_function_node_with_location(name, params, return_type, body, location);
    node->data.function.is_public = 1;  //pub函数
    return node;
}

ASTNode* create_extern_function_node_with_location(const char* name, ASTNode* params, ASTNode* return_type, const char* linkage, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_FUNCTION;
    node->location = location;
    node->data.function.name = malloc(strlen(name) + 1);
    strcpy(node->data.function.name, name);
    node->data.function.params = params;
    node->data.function.generic_params = NULL;
    node->data.function.return_type = return_type;
    node->data.function.body = NULL;
    node->data.function.is_extern = 1;
    if (linkage) {
        node->data.function.linkage = malloc(strlen(linkage) + 1);
        strcpy(node->data.function.linkage, linkage);
    } else {
        node->data.function.linkage = NULL;
    }
    node->data.function.vararg = 0;
    return node;
}

ASTNode* create_extern_function_node(const char* name, ASTNode* params, ASTNode* return_type, const char* linkage) {
    Location loc = {0,0,0,0};
    return create_extern_function_node_with_location(name, params, return_type, linkage, loc);
}

ASTNode* create_break_node_with_location(Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_BREAK;
    node->location = location;
    return node;
}

ASTNode* create_break_node() {
    Location loc = {1, 1, 1, 1};
    return create_break_node_with_location(loc);
}

ASTNode* create_continue_node_with_location(Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_CONTINUE;
    node->location = location;
    return node;
}

ASTNode* create_continue_node() {
    Location loc = {1, 1, 1, 1};
    return create_continue_node_with_location(loc);
}

ASTNode* create_return_node_with_location(ASTNode* expr, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_RETURN;
    node->location = location;
    node->data.return_stmt.expr = expr;
    return node;
}

ASTNode* create_return_node(ASTNode* expr) {
    if (expr) {
        return create_return_node_with_location(expr, expr->location);
    } else {
        Location loc = {1, 1, 1, 1};
        return create_return_node_with_location(expr, loc);
    }
}

ASTNode* create_call_node(ASTNode* func, ASTNode* args) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_CALL;
    node->location = func->location;// 使用函数的位置
    node->data.call.func = func;
    node->data.call.args = args;
    node->data.call.type_args = NULL;
    return node;
}
ASTNode* create_call_node_with_location(ASTNode* func, ASTNode* args, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_CALL;
    node->location = location;
    node->data.call.func = func;
    node->data.call.args = args;
    node->data.call.type_args = NULL;
    return node;
}
ASTNode* create_call_node_with_yyltype(ASTNode* func, ASTNode* args, void* yylloc) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_CALL;
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    node->location.first_line = loc->first_line;
    node->location.first_column = loc->first_column;
    node->location.last_line = loc->last_line;
    node->location.last_column = loc->last_column;
    node->data.call.func = func;
    node->data.call.args = args;
    node->data.call.type_args = NULL;
    return node;
}

ASTNode* create_struct_def_node_with_location(const char* name, ASTNode* fields, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_STRUCT_DEF;
    node->location = location;
    node->data.struct_def.name = malloc(strlen(name) + 1);
    strcpy(node->data.struct_def.name, name);
    node->data.struct_def.fields = fields;
    node->data.struct_def.generic_params = NULL;
    node->data.struct_def.is_public = 0;
    return node;
}

ASTNode* create_struct_def_node(const char* name, ASTNode* fields) {
    Location loc = {1,1,1,1};
    return create_struct_def_node_with_location(name, fields, loc);
}

ASTNode* create_struct_def_node_with_yyltype(const char* name, ASTNode* fields, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = { loc->first_line, loc->first_column, loc->last_line, loc->last_column };
    return create_struct_def_node_with_location(name, fields, location);
}

ASTNode* create_struct_literal_node_with_location(ASTNode* type_name, ASTNode* fields, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_STRUCT_LITERAL;
    node->location = location;
    node->data.struct_literal.type_name = type_name;
    node->data.struct_literal.fields = fields;
    return node;
}

ASTNode* create_struct_literal_node(ASTNode* type_name, ASTNode* fields) {
    Location loc = type_name ? type_name->location : (Location){1,1,1,1};
    return create_struct_literal_node_with_location(type_name, fields, loc);
}

ASTNode* create_struct_literal_node_with_yyltype(ASTNode* type_name, ASTNode* fields, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = { loc->first_line, loc->first_column, loc->last_line, loc->last_column };
    return create_struct_literal_node_with_location(type_name, fields, location);
}

ASTNode* create_type_app_node_with_location(ASTNode* ctor, ASTNode* args, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_TYPE_APP;
    node->location = location;
    node->data.type_app.ctor = ctor;
    node->data.type_app.args = args;
    return node;
}

ASTNode* create_type_app_node(ASTNode* ctor, ASTNode* args) {
    Location loc = ctor ? ctor->location : (Location){1,1,1,1};
    return create_type_app_node_with_location(ctor, args, loc);
}

ASTNode* create_type_app_node_with_yyltype(ASTNode* ctor, ASTNode* args, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = { loc->first_line, loc->first_column, loc->last_line, loc->last_column };
    return create_type_app_node_with_location(ctor, args, location);
}

ASTNode* create_index_node_with_location(ASTNode* target, ASTNode* index, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_INDEX;
    node->location = location;
    node->data.index.target = target;
    node->data.index.index = index;
    return node;
}

ASTNode* create_index_node(ASTNode* target, ASTNode* index) {
    Location loc = {
        target->location.first_line,
        target->location.first_column,
        index->location.last_line,
        index->location.last_column
    };
    return create_index_node_with_location(target, index, loc);
}

ASTNode* create_index_node_with_yyltype(ASTNode* target, ASTNode* index, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_index_node_with_location(target, index, location);
}

ASTNode* create_member_access_node(ASTNode* object, ASTNode* field) {
    Location loc = {
        object->location.first_line,
        object->location.first_column,
        field->location.last_line,
        field->location.last_column
    };
    return create_member_access_node_with_location(object, field, loc);
}

ASTNode* create_member_access_node_with_location(ASTNode* object, ASTNode* field, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_MEMBER_ACCESS;
    node->location = location;
    node->data.member_access.object = object;
    node->data.member_access.field = field;
    return node;
}

ASTNode* create_member_access_node_with_yyltype(ASTNode* object, ASTNode* field, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_member_access_node_with_location(object, field, location);
}

ASTNode* create_toint_node_with_yyltype(ASTNode* expr, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_toint_node_with_location(expr, location);
}

ASTNode* create_tofloat_node_with_yyltype(ASTNode* expr, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_tofloat_node_with_location(expr, location);
}

ASTNode* create_program_node_with_yyltype(void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_program_node_with_location(location);
}

ASTNode* create_return_node_with_yyltype(ASTNode* expr, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_return_node_with_location(expr, location);
}

ASTNode* create_break_node_with_yyltype(void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_break_node_with_location(location);
}

ASTNode* create_continue_node_with_yyltype(void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_continue_node_with_location(location);
}

ASTNode* create_print_node_with_yyltype(ASTNode* expr, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_print_node_with_location(expr, location);
}

ASTNode* create_const_node_with_yyltype(ASTNode* left, ASTNode* right, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_const_node_with_location(left, right, location);
}

ASTNode* create_binop_node_with_yyltype(BinOpType op, ASTNode* left, ASTNode* right, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_binop_node_with_location(op, left, right, location);
}

ASTNode* create_if_node_with_yyltype(ASTNode* condition, ASTNode* then_body, ASTNode* else_body, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_if_node_with_location(condition, then_body, else_body, location);
}

ASTNode* create_while_node_with_yyltype(ASTNode* condition, ASTNode* body, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_while_node_with_location(condition, body, location);
}

ASTNode* create_for_node_with_yyltype(ASTNode* var, ASTNode* start, ASTNode* end, ASTNode* body, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_for_node_with_location(var, start, end, body, location);
}

ASTNode* create_expression_list_node_with_yyltype(void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_expression_list_node_with_location(location);
}

ASTNode* create_num_int_node_with_yyltype(long long value, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_num_int_node_with_location(value, location);
}

ASTNode* create_num_float_node_with_yyltype(double value, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_num_float_node_with_location(value, location);
}

ASTNode* create_string_node_with_yyltype(const char* value, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_string_node_with_location(value, location);
}

ASTNode* create_identifier_node_with_yyltype(const char* name, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_identifier_node_with_location(name, location);
}

ASTNode* create_global_node_with_location(ASTNode* identifier, ASTNode* type, ASTNode* initializer, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_GLOBAL;
    node->location = location;
    node->mutability = MUTABILITY_IMMUTABLE;//global cannt bian
    node->data.global_decl.identifier = identifier;
    node->data.global_decl.type = type;
    node->data.global_decl.initializer = initializer;
    node->data.global_decl.is_public = 0;
    return node;
}

ASTNode* create_global_node(ASTNode* identifier, ASTNode* type, ASTNode* initializer) {
    Location loc = {1, 1, 1, 1};
    if (identifier) loc = identifier->location;
    else if (initializer) loc = initializer->location;
    return create_global_node_with_location(identifier, type, initializer, loc);
}

ASTNode* create_global_node_with_yyltype(ASTNode* identifier, ASTNode* type, ASTNode* initializer, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_global_node_with_location(identifier, type, initializer, location);
}

ASTNode* create_import_node(const char* module_path) {
    Location loc = {0, 0, 0, 0};//默认位置
    return create_import_node_with_location(module_path, loc);
}

ASTNode* create_import_node_with_location(const char* module_path, Location location) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_IMPORT;
    node->location = location;
    node->mutability = MUTABILITY_IMMUTABLE;
    node->data.import.module_path = strdup(module_path);
    return node;
}

ASTNode* create_import_node_with_yyltype(const char* module_path, void* yylloc) {
    YYLTYPE* loc = (YYLTYPE*)yylloc;
    Location location = {
        loc->first_line,
        loc->first_column,
        loc->last_line,
        loc->last_column
    };
    return create_import_node_with_location(module_path, location);
}

ASTNode* create_char_node(char value) {
    ASTNode* node = alloc_ast_node();
    node->type = AST_CHAR;
    node->data.character.value = value;
    node->mutability = MUTABILITY_IMMUTABLE;
    node->location.first_line = 0;
    node->location.first_column = 0;
    node->location.last_line = 0;
    node->location.last_column = 0;
    return node;
}

ASTNode* create_char_node_with_location(char value, Location location) {
    ASTNode* node = create_char_node(value);
    node->location = location;
    return node;
}

ASTNode* create_char_node_with_yyltype(char value, void* yylloc) {
    Location loc = *(Location*)yylloc;
    return create_char_node_with_location(value, loc);
}

void free_ast(ASTNode* node) {
    if (!node) return;
    
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->data.program.statement_count; i++) {
                free_ast(node->data.program.statements[i]);
            }
            free(node->data.program.statements);
            break;
            
        case AST_PRINT:
            free_ast(node->data.print.expr);
            break;
            
        case AST_INPUT:
            free_ast(node->data.input.prompt);
            break;
            
        case AST_TOINT:
            free_ast(node->data.toint.expr);
            break;
            
        case AST_TOFLOAT:
            free_ast(node->data.tofloat.expr);
            break;
            
        case AST_EXPRESSION_LIST:
            for (int i = 0; i < node->data.expression_list.expression_count; i++) {
                free_ast(node->data.expression_list.expressions[i]);
            }
            free(node->data.expression_list.expressions);
            break;
            
        case AST_ASSIGN:
            free_ast(node->data.assign.left);
            free_ast(node->data.assign.right);
            break;
        case AST_CONST:
            free_ast(node->data.assign.left);
            free_ast(node->data.assign.right);
            break;
            
        case AST_BINOP:
            free_ast(node->data.binop.left);
            free_ast(node->data.binop.right);
            break;
            
        case AST_UNARYOP:
            free_ast(node->data.unaryop.expr);
            break;
            
        case AST_STRING:
            free(node->data.string.value);
            break;
            
        case AST_CHAR:
            break;
            
        case AST_IDENTIFIER:
            free(node->data.identifier.name);
            break;
            
        case AST_TYPE_INT32:
        case AST_TYPE_INT64:
        case AST_TYPE_INT8:
        case AST_TYPE_FLOAT32:
        case AST_TYPE_FLOAT64:
        case AST_TYPE_STRING:
        case AST_TYPE_VOID:
        case AST_TYPE_POINTER:
            if (node->type == AST_TYPE_POINTER && node->data.pointer_type.element_type) {
                free_ast(node->data.pointer_type.element_type);
            }
            break;
            
        case AST_TYPE_LIST:
            if (node->data.list_type.element_type) {
                free_ast(node->data.list_type.element_type);
            }
            break;
            
        case AST_TYPE_FIXED_SIZE_LIST:
            if (node->data.fixed_size_list_type.element_type) {
                free_ast(node->data.fixed_size_list_type.element_type);
            }
            break;
        case AST_TYPE_APP:
            if (node->data.type_app.ctor) {
                free_ast(node->data.type_app.ctor);
            }
            if (node->data.type_app.args) {
                free_ast(node->data.type_app.args);
            }
            break;
            
        case AST_IF:
            free_ast(node->data.if_stmt.condition);
            free_ast(node->data.if_stmt.then_body);
            if (node->data.if_stmt.else_body) {
                free_ast(node->data.if_stmt.else_body);
            }
            break;
        case AST_IMPORT:
            free(node->data.import.module_path);
            break;
        case AST_WHILE:
            free_ast(node->data.while_stmt.condition);
            free_ast(node->data.while_stmt.body);
            break;
            
        case AST_FOR:
            free_ast(node->data.for_stmt.var);
            free_ast(node->data.for_stmt.start);
            free_ast(node->data.for_stmt.end);
            free_ast(node->data.for_stmt.body);
            break;
        case AST_INDEX:
            free_ast(node->data.index.target);
            free_ast(node->data.index.index);
            break;
        case AST_MEMBER_ACCESS:
            free_ast(node->data.member_access.object);
            free_ast(node->data.member_access.field);
            break;
            
        case AST_FUNCTION:
            free(node->data.function.name);
            if (node->data.function.params) {
                free_ast(node->data.function.params);
            }
            if (node->data.function.generic_params) {
                free_ast(node->data.function.generic_params);
            }
            if (node->data.function.return_type) {
                free_ast(node->data.function.return_type);
            }
            if (node->data.function.body) {
                free_ast(node->data.function.body);
            }
            if (node->data.function.linkage) {
                free(node->data.function.linkage);
            }
            break;
            
        case AST_CALL:
            free_ast(node->data.call.func);
            if (node->data.call.args) {
                free_ast(node->data.call.args);
            }
            if (node->data.call.type_args) {
                free_ast(node->data.call.type_args);
            }
            break;
        case AST_STRUCT_DEF:
            free(node->data.struct_def.name);
            if (node->data.struct_def.fields) free_ast(node->data.struct_def.fields);
            if (node->data.struct_def.generic_params) free_ast(node->data.struct_def.generic_params);
            break;
        case AST_STRUCT_LITERAL:
            if (node->data.struct_literal.type_name) free_ast(node->data.struct_literal.type_name);
            if (node->data.struct_literal.fields) free_ast(node->data.struct_literal.fields);
            break;
            
        case AST_RETURN:
            if (node->data.return_stmt.expr) {
                free_ast(node->data.return_stmt.expr);
            }
            break;
        case AST_GLOBAL:
            if (node->data.global_decl.identifier) {
                free_ast(node->data.global_decl.identifier);
            }
            if (node->data.global_decl.type) {
                free_ast(node->data.global_decl.type);
            }
            if (node->data.global_decl.initializer) {
                free_ast(node->data.global_decl.initializer);
            }
            break;
            
        case AST_NUM_INT:
        case AST_NUM_FLOAT:
        case AST_NIL:
        case AST_BREAK:
        case AST_CONTINUE:
            break;
    }

    if (node->inferred_type) {
        free_type_info(node->inferred_type);
        node->inferred_type = NULL;
    }
    
    free(node);
}

void print_ast(ASTNode* node, int indent) {
    if (!node) return;
    
    for (int i = 0; i < indent; i++) printf("  ");
    
    switch (node->type) {
        case AST_PROGRAM:
            printf("Program:\n");
            for (int i = 0; i < node->data.program.statement_count; i++) {
                print_ast(node->data.program.statements[i], indent + 1);
            }
            break;
        case AST_PRINT:
            printf("Print:\n");
            print_ast(node->data.print.expr, indent + 1);
            break;
        case AST_ASSIGN:
            printf("Assign:\n");
            print_ast(node->data.assign.left, indent + 1);
            print_ast(node->data.assign.right, indent + 1);
            break;
        case AST_CONST:
            printf("Const:\n");
            print_ast(node->data.assign.left, indent + 1);
            print_ast(node->data.assign.right, indent + 1);
            break;
        case AST_IF:
            printf("If:\n");
            print_ast(node->data.if_stmt.condition, indent + 1);
            print_ast(node->data.if_stmt.then_body, indent + 1);
            if (node->data.if_stmt.else_body) {
                for (int i = 0; i < indent; i++) printf("  ");
                printf("Else:\n");
                print_ast(node->data.if_stmt.else_body, indent + 1);
            }
            break;
        case AST_IMPORT:
            printf("Import: %s\n", node->data.import.module_path);
            break;
        case AST_WHILE:
            printf("While:\n");
            print_ast(node->data.while_stmt.condition, indent + 1);
            print_ast(node->data.while_stmt.body, indent + 1);
            break;
        case AST_FOR:
            printf("For:\n");
            print_ast(node->data.for_stmt.var, indent + 1);
            print_ast(node->data.for_stmt.start, indent + 1);
            print_ast(node->data.for_stmt.end, indent + 1);
            print_ast(node->data.for_stmt.body, indent + 1);
            break;
        case AST_INDEX:
            printf("Index:\n");
            print_ast(node->data.index.target, indent + 1);
            print_ast(node->data.index.index, indent + 1);
            break;
        case AST_MEMBER_ACCESS:
            printf("Member Access:\n");
            print_ast(node->data.member_access.object, indent + 1);
            print_ast(node->data.member_access.field, indent + 1);
            break;
        case AST_BREAK:
            printf("Break\n");
            break;
        case AST_CONTINUE:
            printf("Continue\n");
            break;
        case AST_BINOP:
            printf("Binary Operation (%s):\n", 
                   node->data.binop.op == OP_ADD ? "+" :
                   node->data.binop.op == OP_SUB ? "-" :
                   node->data.binop.op == OP_MUL ? "*" :
                   node->data.binop.op == OP_DIV ? "/" :
                   node->data.binop.op == OP_MOD ? "%" :
                   node->data.binop.op == OP_POW ? "**" :
                   node->data.binop.op == OP_EQ ? "==" :
                   node->data.binop.op == OP_NE ? "!=" :
                   node->data.binop.op == OP_LT ? "<" :
                   node->data.binop.op == OP_LE ? "<=" :
                   node->data.binop.op == OP_GT ? ">" :
                   node->data.binop.op == OP_GE ? ">=" :
                   node->data.binop.op == OP_AND ? "and" :
                   node->data.binop.op == OP_OR ? "or" : "unknown");
            for (int i = 0; i < indent + 1; i++) printf("  ");
            printf("Left:\n");
            print_ast(node->data.binop.left, indent + 2);
            for (int i = 0; i < indent + 1; i++) printf("  ");
            printf("Right:\n");
            print_ast(node->data.binop.right, indent + 2);
            return;
            print_ast(node->data.binop.left, indent + 1);
            print_ast(node->data.binop.right, indent + 1);
            break;
        case AST_UNARYOP:
            {
                const char* op_str;
                switch (node->data.unaryop.op) {
                    case OP_MINUS:   op_str = "-"; break;
                    case OP_PLUS:    op_str = "+"; break;
                    case OP_ADDRESS: op_str = "&"; break;
                    case OP_DEREF:   op_str = "@"; break;
                    default:         op_str = "?"; break;
                }
                printf("Unary Operation (%s):\n", op_str);
                print_ast(node->data.unaryop.expr, indent + 1);
            }
            break;
        case AST_NUM_INT:
            printf("Integer: %lld\n", node->data.num_int.value);
            break;
        case AST_NUM_FLOAT:
            printf("Float: %g\n", node->data.num_float.value);
            break;
        case AST_NIL:
            printf("Nil\n");
            break;
        case AST_STRING:
            printf("String: \"%s\"\n", node->data.string.value);
            break;
        case AST_CHAR:
            printf("Char: '%c'\n", node->data.character.value);
            break;
        case AST_IDENTIFIER:
            printf("Identifier: %s\n", node->data.identifier.name);
            break;
        case AST_INPUT:
            printf("Input:\n");
            print_ast(node->data.input.prompt, indent + 1);
            break;
        case AST_TOINT:
            printf("ToInt:\n");
            print_ast(node->data.toint.expr, indent + 1);
            break;
        case AST_TOFLOAT:
            printf("ToFloat:\n");
            print_ast(node->data.tofloat.expr, indent + 1);
            break;
        case AST_EXPRESSION_LIST:
            printf("Expression List:\n");
            for (int i = 0; i < node->data.expression_list.expression_count; i++) {
                if (node->data.expression_list.expressions[i]->type == AST_ASSIGN &&
                    node->data.expression_list.expressions[i]->data.assign.left->type == AST_IDENTIFIER &&
                    (node->data.expression_list.expressions[i]->data.assign.right->type == AST_TYPE_INT32 ||
                     node->data.expression_list.expressions[i]->data.assign.right->type == AST_TYPE_INT64 ||
                     node->data.expression_list.expressions[i]->data.assign.right->type == AST_TYPE_FLOAT32 ||
                     node->data.expression_list.expressions[i]->data.assign.right->type == AST_TYPE_FLOAT64 ||
                     node->data.expression_list.expressions[i]->data.assign.right->type == AST_TYPE_STRING ||
                     node->data.expression_list.expressions[i]->data.assign.right->type == AST_TYPE_VOID)) {
                    for (int j = 0; j < indent + 1; j++) printf("  ");
                    printf("Annotated Parameter: %s : ", 
                           node->data.expression_list.expressions[i]->data.assign.left->data.identifier.name);
                    
                    switch(node->data.expression_list.expressions[i]->data.assign.right->type) {
                        case AST_TYPE_INT32: printf("i32\n"); break;
                        case AST_TYPE_INT64: printf("i64\n"); break;
                        case AST_TYPE_INT8: printf("i8\n"); break;
                        case AST_TYPE_FLOAT32: printf("f32\n"); break;
                        case AST_TYPE_FLOAT64: printf("f64\n"); break;
                        case AST_TYPE_STRING: printf("string\n"); break;
                        case AST_TYPE_VOID: printf("void\n"); break;
                        default: printf("unknown\n"); break;
                    }
                } else {
                    print_ast(node->data.expression_list.expressions[i], indent + 1);
                }
            }
            break;
        case AST_TYPE_INT32:
            printf("Type: i32\n");
            break;
        case AST_TYPE_INT64:
            printf("Type: i64\n");
            break;
        case AST_TYPE_INT8:
            printf("Type: i8\n");
            break;
        case AST_TYPE_FLOAT32:
            printf("Type: f32\n");
            break;
        case AST_TYPE_FLOAT64:
            printf("Type: f64\n");
            break;
        case AST_TYPE_STRING:
            printf("Type: string\n");
            break;
        case AST_TYPE_VOID:
            printf("Type: void\n");
            break;
        case AST_TYPE_POINTER:
            printf("Type: ptr\n");
            break;
        case AST_TYPE_LIST:
            printf("Type: List of ");
            if (node->data.list_type.element_type) {
                switch(node->data.list_type.element_type->type) {
                    case AST_TYPE_INT32: printf("i32"); break;
                    case AST_TYPE_INT64: printf("i64"); break;
                    case AST_TYPE_INT8: printf("i8"); break;  // 添加对AST_TYPE_INT8的支持
                    case AST_TYPE_FLOAT32: printf("f32"); break;
                    case AST_TYPE_FLOAT64: printf("f64"); break;
                    case AST_TYPE_STRING: printf("string"); break;
                    case AST_TYPE_VOID: printf("void"); break;
                    case AST_IDENTIFIER: printf("%s", node->data.list_type.element_type->data.identifier.name); break;
                    default: printf("unknown"); break;
                }
            }
            printf("\n");
            break;
        case AST_TYPE_FIXED_SIZE_LIST:
            printf("Type: [");
            if (node->data.fixed_size_list_type.element_type) {
                switch (node->data.fixed_size_list_type.element_type->type) {
                    case AST_TYPE_INT32: printf("i32"); break;
                    case AST_TYPE_INT64: printf("i64"); break;
                    case AST_TYPE_INT8: printf("i8"); break;
                    case AST_TYPE_FLOAT32: printf("f32"); break;
                    case AST_TYPE_FLOAT64: printf("f64"); break;
                    case AST_TYPE_STRING: printf("string"); break;
                    case AST_TYPE_VOID: printf("void"); break;
                    case AST_TYPE_POINTER: printf("ptr"); break;
                    case AST_IDENTIFIER: printf("%s", node->data.fixed_size_list_type.element_type->data.identifier.name); break;
                    default: printf("unknown"); break;
                }
            } else {
                printf("unknown");
            }
            printf(" * %lld]\n", node->data.fixed_size_list_type.size);
            break;
        case AST_TYPE_APP:
            printf("TypeApp: ");
            if (node->data.type_app.ctor && node->data.type_app.ctor->type == AST_IDENTIFIER) {
                printf("%s", node->data.type_app.ctor->data.identifier.name);
            } else {
                printf("unknown");
            }
            if (node->data.type_app.args) {
                printf("[");
                print_ast(node->data.type_app.args, indent + 1);
                printf("]");
            }
            printf("\n");
            break;
        case AST_FUNCTION:
            if (node->data.function.is_extern) {
                printf("Extern Function: %s", node->data.function.name);
                if (node->data.function.linkage) {
                    printf(" (linkage: \"%s\")", node->data.function.linkage);
                }
                if (node->data.function.vararg) printf(" [vararg]");
                printf("\n");
            } else {
                if (node->data.function.is_public) {
                    printf("Public Function: %s\n", node->data.function.name);
                } else {
                    printf("Function: %s\n", node->data.function.name);
                }
            }
            for (int i = 0; i < indent + 1; i++) printf("  ");
            printf("Generic Params:\n");
            if (node->data.function.generic_params) {
                print_ast(node->data.function.generic_params, indent + 2);
            }
            for (int i = 0; i < indent + 1; i++) printf("  ");
            printf("Params:\n");
            if (node->data.function.params) {
                print_ast(node->data.function.params, indent + 2);
            }
            for (int i = 0; i < indent + 1; i++) printf("  ");
            printf("Return Type:\n");
            if (node->data.function.return_type) print_ast(node->data.function.return_type, indent + 2);
            for (int i = 0; i < indent + 1; i++) printf("  ");
            printf("Body:\n");
            if (node->data.function.body) print_ast(node->data.function.body, indent + 2);
            break;
        case AST_CALL:
            printf("Call:\n");
            if (node->data.call.func) print_ast(node->data.call.func, indent + 1);
            if (node->data.call.type_args) {
                for (int i = 0; i < indent + 1; i++) printf("  ");
                printf("Type Args:\n");
                print_ast(node->data.call.type_args, indent + 2);
            }
            if (node->data.call.args) print_ast(node->data.call.args, indent + 1);
            break;
        case AST_STRUCT_DEF:
            if (node->data.struct_def.is_public) {
                printf("Public StructDef: %s\n", node->data.struct_def.name);
            } else {
                printf("StructDef: %s\n", node->data.struct_def.name);
            }
            if (node->data.struct_def.generic_params) print_ast(node->data.struct_def.generic_params, indent + 1);
            if (node->data.struct_def.fields) print_ast(node->data.struct_def.fields, indent + 1);
            break;
        case AST_STRUCT_LITERAL:
            printf("StructLiteral: type %s\n", node->data.struct_literal.type_name && node->data.struct_literal.type_name->type == AST_IDENTIFIER ? node->data.struct_literal.type_name->data.identifier.name : "unknown");
            if (node->data.struct_literal.fields) print_ast(node->data.struct_literal.fields, indent + 1);
            break;
        case AST_RETURN:
            printf("Return:\n");
            if (node->data.return_stmt.expr) print_ast(node->data.return_stmt.expr, indent + 1);
            break;
        case AST_GLOBAL:
            if (node->data.global_decl.is_public) {
                printf("Public Global Declaration:\n");
            } else {
                printf("Global Declaration:\n");
            }
            if (node->data.global_decl.identifier) {
                for (int i = 0; i < indent + 1; i++) printf("  ");
                printf("Identifier: ");
                print_ast(node->data.global_decl.identifier, 0);
            }
            if (node->data.global_decl.type) {
                for (int i = 0; i < indent + 1; i++) printf("  ");
                printf("Type: ");
                print_ast(node->data.global_decl.type, 0);
            }
            if (node->data.global_decl.initializer) {
                for (int i = 0; i < indent + 1; i++) printf("  ");
                printf("Initializer:\n");
                print_ast(node->data.global_decl.initializer, indent + 2);
            }
            break;
        default:
            printf("Unknown node type!!!: %d\n", node->type);
            break;
    }
}
static void free_inserted_externs(char** inserted_externs, int inserted_extern_count) {
    if (!inserted_externs) return;
    for (int z = 0; z < inserted_extern_count; z++) {
        free(inserted_externs[z]);
    }
    free(inserted_externs);
}

static int append_inserted_extern(char*** inserted_externs, int* inserted_extern_count, const char* name) {
    if (!inserted_externs || !inserted_extern_count || !name) return 1;

    for (int i = 0; i < *inserted_extern_count; i++) {
        if ((*inserted_externs)[i] && strcmp((*inserted_externs)[i], name) == 0) {
            return 1;
        }
    }

    char** resized = realloc(*inserted_externs, sizeof(char*) * (*inserted_extern_count + 1));
    if (!resized) return 0;

    char* name_copy = strdup(name);
    if (!name_copy) {
        *inserted_externs = resized;
        return 0;
    }

    *inserted_externs = resized;
    resized[*inserted_extern_count] = name_copy;
    (*inserted_extern_count)++;
    return 1;
}

static int inline_imports_in_node_impl(ASTNode* node) {
    if (!node) return 1;

    if (node->type == AST_PROGRAM) {
        int i = 0;
        int inserted_extern_count = 0;
        char** inserted_externs = NULL;

        for (int z = 0; z < node->data.program.statement_count; z++) {
            ASTNode* s = node->data.program.statements[z];
            if (s && s->type == AST_FUNCTION && s->data.function.is_extern && s->data.function.name) {
                if (!append_inserted_extern(&inserted_externs, &inserted_extern_count, s->data.function.name)) {
                    free_inserted_externs(inserted_externs, inserted_extern_count);
                    return 0;
                }
            }
        }

        while (i < node->data.program.statement_count) {
            ASTNode* stmt = node->data.program.statements[i];
            if (!stmt) {
                i++;
                continue;
            }

            if (stmt->type == AST_IMPORT) {
                const char* module_path = stmt->data.import.module_path;
                char full_module_path[1024];
                if (!resolve_import_path(current_input_filename, module_path, full_module_path, sizeof(full_module_path))) {
                    i++;
                    continue;
                }

                if (import_cache_contains(full_module_path)) {
                    remove_program_statement_at(node, i);
                    continue;
                }

                import_cache_add(full_module_path);
                ASTNode* module_root = parse_imported_module_ast(full_module_path);
                if (!module_root) {
                    i++;
                    continue;
                }

                char* module_source_file = strdup(full_module_path);
                if (!module_source_file) {
                    free_ast(module_root);
                    free_inserted_externs(inserted_externs, inserted_extern_count);
                    return 0;
                }

                set_source_file_recursive(module_root, module_source_file);

                const char* old_inline_current = current_input_filename;
                current_input_filename = module_source_file;
                if (!inline_imports_in_node_impl(module_root)) {
                    current_input_filename = old_inline_current;
                    free_ast(module_root);
                    free_inserted_externs(inserted_externs, inserted_extern_count);
                    return 0;
                }
                current_input_filename = old_inline_current;

                if (module_root->type != AST_PROGRAM) {
                    free_ast(module_root);
                    i++;
                    continue;
                }

                int add_count = 0;
                int extern_count_before_import = inserted_extern_count;
                for (int j = 0; j < module_root->data.program.statement_count; j++) {
                    ASTNode* s = module_root->data.program.statements[j];
                    if (!s) continue;
                    if (s->type == AST_FUNCTION) {
                        if (s->data.function.is_public) {
                            add_count++;
                        } else if (s->data.function.is_extern && s->data.function.name) {
                            int found = 0;
                            for (int e = 0; e < inserted_extern_count; e++) {
                                if (strcmp(inserted_externs[e], s->data.function.name) == 0) {
                                    found = 1;
                                    break;
                                }
                            }
                            if (!found) {
                                if (!append_inserted_extern(&inserted_externs, &inserted_extern_count, s->data.function.name)) {
                                    free_ast(module_root);
                                    free_inserted_externs(inserted_externs, inserted_extern_count);
                                    return 0;
                                }
                                add_count++;
                            }
                        }
                    } else if (s->type == AST_CONST) {
                        if (s->data.assign.is_public) {
                            add_count++;
                        }
                    } else if (s->type == AST_STRUCT_DEF) {
                        if (s->data.struct_def.is_public) {
                            add_count++;
                        }
                    } else if (s->type == AST_GLOBAL) {
                        if (s->data.global_decl.is_public) {
                            add_count++;
                        }
                    } else if (s->type == AST_PROGRAM) {
                        for (int jj = 0; jj < s->data.program.statement_count; jj++) {
                            ASTNode* t = s->data.program.statements[jj];
                            if (!t) continue;
                            if (t->type == AST_FUNCTION) {
                                if (t->data.function.is_public) {
                                    add_count++;
                                } else if (t->data.function.is_extern && t->data.function.name) {
                                    int found = 0;
                                    for (int e = 0; e < inserted_extern_count; e++) {
                                        if (strcmp(inserted_externs[e], t->data.function.name) == 0) {
                                            found = 1;
                                            break;
                                        }
                                    }
                                    if (!found) {
                                        if (!append_inserted_extern(&inserted_externs, &inserted_extern_count, t->data.function.name)) {
                                            free_ast(module_root);
                                            free_inserted_externs(inserted_externs, inserted_extern_count);
                                            return 0;
                                        }
                                        add_count++;
                                    }
                                }
                            } else if (t->type == AST_CONST) {
                                if (t->data.assign.is_public) {
                                    add_count++;
                                }
                            } else if (t->type == AST_STRUCT_DEF) {
                                if (t->data.struct_def.is_public) {
                                    add_count++;
                                }
                            } else if (t->type == AST_GLOBAL) {
                                if (t->data.global_decl.is_public) {
                                    add_count++;
                                }
                            }
                        }
                    }
                }

                if (add_count == 0) {
                    free_ast(module_root);
                    i++;
                    continue;
                }

                int old_count = node->data.program.statement_count;
                int new_count = old_count - 1 + add_count;
                ASTNode** new_statements = malloc(sizeof(ASTNode*) * new_count);
                if (!new_statements) {
                    free_ast(module_root);
                    free_inserted_externs(inserted_externs, inserted_extern_count);
                    return 0;
                }

                int idx = 0;
                for (int k = 0; k < i; k++) {
                    new_statements[idx++] = node->data.program.statements[k];//复制
                }
                for (int j = 0; j < module_root->data.program.statement_count; j++) {
                    ASTNode* s = module_root->data.program.statements[j];
                    if (!s) continue;
                    if (s->type == AST_FUNCTION) {
                        if (s->data.function.is_public) {
                            new_statements[idx++] = s;
                            module_root->data.program.statements[j] = NULL;//no double free
                        } else if (s->data.function.is_extern && s->data.function.name) {
                            int already_moved = 0;
                            for (int p = 0; p < idx; p++) {
                                ASTNode* moved = new_statements[p];
                                if (moved && moved->type == AST_FUNCTION && moved->data.function.is_extern && moved->data.function.name && strcmp(moved->data.function.name, s->data.function.name) == 0) {
                                    already_moved = 1;
                                    break;
                                }
                            }
                            if (!already_moved) {
                                for (int e = 0; e < extern_count_before_import; e++) {
                                    if (strcmp(inserted_externs[e], s->data.function.name) == 0) {
                                        already_moved = 1;
                                        break;
                                    }
                                }
                            }
                            if (!already_moved) {
                                new_statements[idx++] = s;
                                module_root->data.program.statements[j] = NULL;
                            }
                        }
                    } else if (s->type == AST_CONST) {
                        if (s->data.assign.is_public) {
                            new_statements[idx++] = s;
                            module_root->data.program.statements[j] = NULL;
                        }
                    } else if (s->type == AST_STRUCT_DEF) {
                        if (s->data.struct_def.is_public) {
                            new_statements[idx++] = s;
                            module_root->data.program.statements[j] = NULL;
                        }
                    } else if (s->type == AST_GLOBAL) {
                        if (s->data.global_decl.is_public) {
                            new_statements[idx++] = s;
                            module_root->data.program.statements[j] = NULL;
                        }
                    } else if (s->type == AST_PROGRAM) {
                        for (int jj = 0; jj < s->data.program.statement_count; jj++) {
                            ASTNode* t = s->data.program.statements[jj];
                            if (!t) continue;
                            if (t->type == AST_FUNCTION) {
                                if (t->data.function.is_public) {
                                    new_statements[idx++] = t;
                                    s->data.program.statements[jj] = NULL;
                                } else if (t->data.function.is_extern && t->data.function.name) {
                                    int already_moved = 0;
                                    for (int p = 0; p < idx; p++) {
                                        ASTNode* moved = new_statements[p];
                                        if (moved && moved->type == AST_FUNCTION && moved->data.function.is_extern && moved->data.function.name && strcmp(moved->data.function.name, t->data.function.name) == 0) {
                                            already_moved = 1;
                                            break;
                                        }
                                    }
                                    if (!already_moved) {
                                        for (int e = 0; e < extern_count_before_import; e++) {
                                            if (strcmp(inserted_externs[e], t->data.function.name) == 0) {
                                                already_moved = 1;
                                                break;
                                            }
                                        }
                                    }
                                    if (!already_moved) {
                                        new_statements[idx++] = t;
                                        s->data.program.statements[jj] = NULL;
                                    }
                                }
                            } else if (t->type == AST_CONST) {
                                if (t->data.assign.is_public) {
                                    new_statements[idx++] = t;
                                    s->data.program.statements[jj] = NULL;
                                }
                            } else if (t->type == AST_STRUCT_DEF) {
                                if (t->data.struct_def.is_public) {
                                    new_statements[idx++] = t;
                                    s->data.program.statements[jj] = NULL;
                                }
                            } else if (t->type == AST_GLOBAL) {
                                if (t->data.global_decl.is_public) {
                                    new_statements[idx++] = t;
                                    s->data.program.statements[jj] = NULL;
                                }
                            }
                        }
                    }
                }
                for (int k = i + 1; k < old_count; k++) {
                    new_statements[idx++] = node->data.program.statements[k];//复制语句
                }

                free(node->data.program.statements);
                node->data.program.statements = new_statements;
                node->data.program.statement_count = new_count;
                free_ast(stmt);
                free_ast(module_root);
                i += add_count; //next
            } else {
                if (!inline_imports_in_node_impl(stmt)) {
                    free_inserted_externs(inserted_externs, inserted_extern_count);
                    return 0;
                }
                i++;
            }
        }

        /* cleanup inserted_externs */
        free_inserted_externs(inserted_externs, inserted_extern_count);
        return 1;
    }

    if (node->type == AST_FUNCTION) {
        if (node->data.function.body && !inline_imports_in_node_impl(node->data.function.body)) {
            return 0;
        }
        return 1;
    }

    return 1;
}

static void inline_imports_in_node(ASTNode* node) {
    (void)inline_imports_in_node_impl(node);
}

void inline_imports(ASTNode* node) {
    clear_import_cache();
    if (current_input_filename) {
        char root_file[1024];
        if (canonicalize_existing_path(current_input_filename, root_file, sizeof(root_file))) {
            import_cache_add(root_file);
        }
    }
    inline_imports_in_node(node);
    clear_import_cache();
}

int get_array_length(ASTNode* node) {
    if (!node || node->type != AST_EXPRESSION_LIST) {
        return -1;
    }
    if (node->data.expression_list.precomputed_length >= 0) {
        return node->data.expression_list.precomputed_length;
    }
    node->data.expression_list.precomputed_length = node->data.expression_list.expression_count;
    return node->data.expression_list.precomputed_length;
}