#include "../include/typeinfer.h"
#include "../include/compiler.h"
#include "../include/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void* checked_realloc(void* ptr, size_t size) {
    void* resized = realloc(ptr, size);
    if (!resized && size != 0) {
        fprintf(stderr, "Failed to reallocate %zu bytes\n", size);
        exit(1);
    }
    return resized;
}

extern const char* current_input_filename;
static InferredType infer_index_type(TypeInferenceContext* ctx, ASTNode* node);
static InferredType get_nested_field_type(TypeInferenceContext* ctx, ASTNode* node) {
    if (!node) return TYPE_UNKNOWN;
    if (node->type == AST_INDEX && 
        node->data.index.target->type == AST_IDENTIFIER && 
        node->data.index.index->type == AST_IDENTIFIER) {
        const char* obj_name = node->data.index.target->data.identifier.name;
        const char* field_name = node->data.index.index->data.identifier.name;
        InferredType obj_type = get_variable_type(ctx, obj_name);
        if (obj_type != TYPE_STRUCT) return TYPE_UNKNOWN;
        for (int i = 0; i < ctx->count; i++) {
            if (strcmp(ctx->variables[i].name, obj_name) == 0 && 
                ctx->variables[i].type == TYPE_STRUCT && 
                ctx->variables[i].struct_type) {
                for (int j = 0; j < ctx->variables[i].struct_type->field_count; j++) {
                    if (strcmp(ctx->variables[i].struct_type->fields[j].name, field_name) == 0) {
                        return ctx->variables[i].struct_type->fields[j].type;
                    }
                }
                break;
            }
        }
        return TYPE_UNKNOWN;
    }
    else if (node->type == AST_INDEX && 
             node->data.index.target->type == AST_INDEX && 
             node->data.index.index->type == AST_IDENTIFIER) {
        InferredType nested_type = get_nested_field_type(ctx, node->data.index.target);
        if (nested_type != TYPE_STRUCT) return nested_type;
        const char* field_name = node->data.index.index->data.identifier.name;
        for (int i = 0; i < ctx->count; i++) {
            if (ctx->variables[i].type == TYPE_STRUCT && ctx->variables[i].struct_type) {
                for (int j = 0; j < ctx->variables[i].struct_type->field_count; j++) {
                    if (strcmp(ctx->variables[i].struct_type->fields[j].name, field_name) == 0) {
                        return ctx->variables[i].struct_type->fields[j].type;
                    }
                }
            }
        }
        
        return TYPE_UNKNOWN;
    }
    
    return TYPE_UNKNOWN;
}


TypeInferenceContext* create_type_inference_context() {
    TypeInferenceContext* ctx = malloc(sizeof(TypeInferenceContext));
    ctx->variables = NULL;
    ctx->count = 0;
    ctx->capacity = 0;
    return ctx;
}

void free_type_inference_context(TypeInferenceContext* ctx) {
    if (!ctx) return;
    
    for (int i = 0; i < ctx->count; i++) {
        free(ctx->variables[i].name);
        if (ctx->variables[i].struct_type) {
            free_struct_type(ctx->variables[i].struct_type);
        }
    }
    free(ctx->variables);
    free(ctx);
}

InferredType infer_type(TypeInferenceContext* ctx, ASTNode* node) {
    if (!node) return TYPE_UNKNOWN;
    
    switch (node->type) {
        case AST_CHAR:
            return TYPE_INT8;
            
        case AST_NUM_INT:
            return TYPE_INT;
            
        case AST_NUM_FLOAT:
            return TYPE_FLOAT;
            
        case AST_STRING:
            return TYPE_STRING;
            
        case AST_IDENTIFIER: {
            InferredType type = get_variable_type(ctx, node->data.identifier.name);
            if (type != TYPE_UNKNOWN) {
                return type;
            } else {
                for (int i = 0; i < ctx->count; i++) {
                    if (strcmp(ctx->variables[i].name, node->data.identifier.name) == 0 && 
                        ctx->variables[i].type == TYPE_STRUCT) {
                        return TYPE_STRUCT;
                    }
                }
                report_undefined_variable_with_location(
                    node->data.identifier.name,
                    current_input_filename ? current_input_filename : "unknown",
                    node->location.first_line
                );
                return TYPE_UNKNOWN;
            }
        }
            
        case AST_BINOP: {
            InferredType left_type = infer_type(ctx, node->data.binop.left);
            InferredType right_type = infer_type(ctx, node->data.binop.right);
            if (node->data.binop.op == OP_AND || node->data.binop.op == OP_OR) {
                return TYPE_BOOL;
            }
            
            if (left_type == TYPE_STRING || right_type == TYPE_STRING) {
                return TYPE_STRING;
            }
            if (left_type == TYPE_FLOAT || right_type == TYPE_FLOAT) {
                return TYPE_FLOAT;
            }
            if (left_type == TYPE_INT || right_type == TYPE_INT) {
                if (node->data.binop.op == OP_DIV) {
                    return TYPE_FLOAT;
                }
                return TYPE_INT;
            }
            
            return TYPE_UNKNOWN;
        }
            
        case AST_UNARYOP: {
            if (node->data.unaryop.op == OP_DEREF) {
                ASTNode* inner = node->data.unaryop.expr;// 对于解引用操作，判断内部表达式的类型
                if (inner && inner->type == AST_UNARYOP && inner->data.unaryop.op == OP_ADDRESS) {
                    return infer_type(ctx, inner->data.unaryop.expr);// 如果是 @(&x) 形式，返回 x 的类型
                } else if (inner && inner->type == AST_BINOP && inner->data.binop.op == OP_ADD) {// 如果是 @(ptr + offset) 形式，需要获取ptr指向的类型
                    ASTNode* left = inner->data.binop.left;
                    ASTNode* right = inner->data.binop.right;
                    ASTNode* ptr_expr = NULL;
                    if (left && left->type == AST_UNARYOP && left->data.unaryop.op == OP_ADDRESS) {
                        ptr_expr = left->data.unaryop.expr;
                    } else if (right && right->type == AST_UNARYOP && right->data.unaryop.op == OP_ADDRESS) {
                        ptr_expr = right->data.unaryop.expr;
                    } else if (left && left->type == AST_IDENTIFIER) {
                        ptr_expr = left;
                    }
                    
                    if (ptr_expr) {
                        return infer_type(ctx, ptr_expr);
                    }
                }
                return infer_type(ctx, node->data.unaryop.expr);// 默认情况：返回内部表达式的类型
            } else if (node->data.unaryop.op == OP_ADDRESS) {
                /*
                对于取地址操作，返回指针类型，指向被取地址的对象的类型
                inner_type = infer_type(ctx, node->data.unaryop.expr);
                这里需要存储指针类型和它指向的类型
                暂时返回内部表达式的类型，但实际实现需要更复杂的类型系统
                */
                return TYPE_POINTER;
            } else {
                return infer_type(ctx, node->data.unaryop.expr);
            }
        }
        
        case AST_INPUT: {
            return TYPE_STRING;
        }
        case AST_EXPRESSION_LIST: {
            if (node->data.expression_list.expression_count == 0) return TYPE_LIST;
            return TYPE_LIST;
        }
        case AST_INDEX: {
            return infer_index_type(ctx, node);
        }
        case AST_CALL: {
            if (node->data.call.func && node->data.call.func->type == AST_INDEX) {
                ASTNode* target = node->data.call.func->data.index.target;
                ASTNode* method = node->data.call.func->data.index.index;
                if (method && method->type == AST_IDENTIFIER) {
                    const char* mname = method->data.identifier.name;
                    InferredType elem_type = TYPE_UNKNOWN;
                    InferredType targ_type = infer_type(ctx, target);
                    if (targ_type == TYPE_LIST) {
                        if (target->type == AST_IDENTIFIER) {
                            const char* name = target->data.identifier.name;
                            for (int i = 0; i < ctx->count; i++) {
                                if (strcmp(ctx->variables[i].name, name) == 0) {
                                    elem_type = ctx->variables[i].element_type;
                                    break;
                                }
                            }
                        } else if (target->type == AST_EXPRESSION_LIST) {
                            if (target->data.expression_list.expression_count > 0) {
                                elem_type = infer_type(ctx, target->data.expression_list.expressions[0]);
                            }
                        }
                    }

                    if (strcmp(mname, "remove") == 0 || strcmp(mname, "pop") == 0) {
                        
                        return elem_type != TYPE_UNKNOWN ? elem_type : TYPE_STRING;// 确保pop/remove返回元素类型而不是列表类型
                    }
                    if (strcmp(mname, "add!") == 0 || strcmp(mname, "push!") == 0 || strcmp(mname, "replace!") == 0 || 
                        strcmp(mname, "push") == 0 || strcmp(mname, "remove!") == 0 || strcmp(mname, "pop!") == 0) {
                        return TYPE_UNKNOWN;// 表示这是一个原地修改操作
                    }
                }
            }
            return TYPE_UNKNOWN;
        }
        
        case AST_TOINT: {
            return TYPE_INT;
        }
        
        case AST_TOFLOAT: {
            return TYPE_FLOAT;
        }
        
        case AST_STRUCT_DEF: {
            process_struct_definition(ctx, node);
            return TYPE_UNKNOWN;
        }
        
        case AST_STRUCT_LITERAL: {
            if (node->data.struct_literal.type_name && node->data.struct_literal.type_name->type == AST_IDENTIFIER) {
                const char* struct_name = node->data.struct_literal.type_name->data.identifier.name;
                StructTypeInfo* struct_type = get_struct_type(ctx, struct_name);
                if (struct_type) {
                    return TYPE_STRUCT;
                }
            }
            return TYPE_UNKNOWN;
        }
        
        case AST_ASSIGN: {
            InferredType right_type = infer_type(ctx, node->data.assign.right);
            if (node->data.assign.left->type == AST_IDENTIFIER) {
                if (right_type == TYPE_LIST && node->data.assign.right && node->data.assign.right->type == AST_EXPRESSION_LIST) {
                    InferredType elem_type = TYPE_UNKNOWN;
                    if (node->data.assign.right->data.expression_list.expression_count > 0) {
                        elem_type = infer_type(ctx, node->data.assign.right->data.expression_list.expressions[0]);
                    }
                    set_variable_list_type(ctx, node->data.assign.left->data.identifier.name, TYPE_LIST, elem_type);
                    for (int i = 0; i < ctx->count; i++) {
                        if (strcmp(ctx->variables[i].name, node->data.assign.left->data.identifier.name) == 0) {
                            ctx->variables[i].array_length = node->data.assign.right->data.expression_list.expression_count;
                            break;
                        }
                    }
                } else if (right_type == TYPE_POINTER) {
                    if (node->data.assign.right && node->data.assign.right->type == AST_UNARYOP && node->data.assign.right->data.unaryop.op == OP_ADDRESS) {
                        ASTNode* inner = node->data.assign.right->data.unaryop.expr;
                        InferredType target = infer_type(ctx, inner);
                        set_variable_pointer_type(ctx, node->data.assign.left->data.identifier.name, target);
                    } else {
                        set_variable_pointer_type(ctx, node->data.assign.left->data.identifier.name, TYPE_UNKNOWN);
                    }
                } else if (right_type == TYPE_STRUCT) {
                    if (node->data.assign.right->type == AST_STRUCT_LITERAL) {
                        if (node->data.assign.right->data.struct_literal.type_name && 
                            node->data.assign.right->data.struct_literal.type_name->type == AST_IDENTIFIER) {
                            
                            const char* struct_name = node->data.assign.right->data.struct_literal.type_name->data.identifier.name;
                            StructTypeInfo* struct_type = get_struct_type(ctx, struct_name);
                            if (struct_type) {
                                set_variable_struct_type(ctx, node->data.assign.left->data.identifier.name, struct_type);
                            }
                        }
                    }
                } else {
                    set_variable_type(ctx, node->data.assign.left->data.identifier.name, right_type);
                }
            }
            return right_type;
        }
        case AST_CONST: {
            InferredType right_type = infer_type(ctx, node->data.assign.right);
            if (node->data.assign.left->type == AST_IDENTIFIER) {
                set_variable_type(ctx, node->data.assign.left->data.identifier.name, right_type);
            }
            return right_type;
        }
            
        default:
            return TYPE_UNKNOWN;
    }
}

void set_variable_list_type(TypeInferenceContext* ctx, const char* var_name, InferredType list_type, InferredType element_type) {
    if (!ctx || !var_name) return;
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->variables[i].name, var_name) == 0) {
            ctx->variables[i].type = list_type;
            ctx->variables[i].element_type = element_type;
            ctx->variables[i].array_length = -1;
            return;
        }
    }
    if (ctx->count >= ctx->capacity) {
        ctx->capacity = ctx->capacity == 0 ? 10 : ctx->capacity * 2;
        ctx->variables = checked_realloc(ctx->variables, sizeof(VariableInfo) * ctx->capacity);
    }
    ctx->variables[ctx->count].name = malloc(strlen(var_name) + 1);
    strcpy(ctx->variables[ctx->count].name, var_name);
    ctx->variables[ctx->count].type = list_type;
    ctx->variables[ctx->count].element_type = element_type;
    ctx->variables[ctx->count].pointer_target_type = TYPE_UNKNOWN;
    ctx->variables[ctx->count].struct_type = NULL;
    ctx->variables[ctx->count].array_length = -1;
    ctx->count++;
}

InferredType get_variable_type(TypeInferenceContext* ctx, const char* var_name) {
    if (!ctx || !var_name) return TYPE_UNKNOWN;
    
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->variables[i].name, var_name) == 0) {
            return ctx->variables[i].type;
        }
    }
    
    return TYPE_UNKNOWN;
}

void set_variable_type(TypeInferenceContext* ctx, const char* var_name, InferredType type) {
    if (!ctx || !var_name) return;
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->variables[i].name, var_name) == 0) {
            ctx->variables[i].type = type;
            ctx->variables[i].element_type = TYPE_UNKNOWN;
            ctx->variables[i].pointer_target_type = TYPE_UNKNOWN;//重置指针类型
            ctx->variables[i].struct_type = NULL;
            ctx->variables[i].array_length = -1;
            return;
        }
    }
    if (ctx->count >= ctx->capacity) {
        ctx->capacity = ctx->capacity == 0 ? 10 : ctx->capacity * 2;
        ctx->variables = checked_realloc(ctx->variables, sizeof(VariableInfo) * ctx->capacity);
    }
    
    ctx->variables[ctx->count].name = malloc(strlen(var_name) + 1);
    strcpy(ctx->variables[ctx->count].name, var_name);
    ctx->variables[ctx->count].type = type;
    ctx->variables[ctx->count].element_type = TYPE_UNKNOWN;
    ctx->variables[ctx->count].pointer_target_type = TYPE_UNKNOWN;
    ctx->variables[ctx->count].struct_type = NULL;
    ctx->variables[ctx->count].array_length = -1;
    ctx->count++;
}

void set_variable_pointer_type(TypeInferenceContext* ctx, const char* var_name, InferredType target_type) {
    if (!ctx || !var_name) return;
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->variables[i].name, var_name) == 0) {
            ctx->variables[i].type = TYPE_POINTER;
            ctx->variables[i].pointer_target_type = target_type;
            ctx->variables[i].element_type = TYPE_UNKNOWN;
            ctx->variables[i].struct_type = NULL;
            ctx->variables[i].array_length = -1;
            return;
        }
    }
    if (ctx->count >= ctx->capacity) {
        ctx->capacity = ctx->capacity == 0 ? 10 : ctx->capacity * 2;
        ctx->variables = checked_realloc(ctx->variables, sizeof(VariableInfo) * ctx->capacity);
    }
    
    ctx->variables[ctx->count].name = malloc(strlen(var_name) + 1);
    strcpy(ctx->variables[ctx->count].name, var_name);
    ctx->variables[ctx->count].type = TYPE_POINTER;
    ctx->variables[ctx->count].element_type = TYPE_UNKNOWN;
    ctx->variables[ctx->count].pointer_target_type = target_type;
    ctx->variables[ctx->count].struct_type = NULL;
    ctx->variables[ctx->count].array_length = -1;
    ctx->count++;
}

InferredType get_variable_pointer_target_type(TypeInferenceContext* ctx, const char* var_name) {
    if (!ctx || !var_name) return TYPE_UNKNOWN;
    
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->variables[i].name, var_name) == 0) {
            return ctx->variables[i].pointer_target_type;
        }
    }
    
    return TYPE_UNKNOWN;
}
void set_variable_struct_type(TypeInferenceContext* ctx, const char* var_name, StructTypeInfo* struct_type) {
    if (!ctx || !var_name || !struct_type) return;
    
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->variables[i].name, var_name) == 0) {
            ctx->variables[i].type = TYPE_STRUCT;
            ctx->variables[i].struct_type = struct_type;
            ctx->variables[i].element_type = TYPE_UNKNOWN;
            ctx->variables[i].pointer_target_type = TYPE_UNKNOWN;
            ctx->variables[i].array_length = -1;
            return;
        }
    }
    
    if (ctx->count >= ctx->capacity) {
        ctx->capacity = ctx->capacity == 0 ? 10 : ctx->capacity * 2;
        ctx->variables = checked_realloc(ctx->variables, sizeof(VariableInfo) * ctx->capacity);
    }
    
    ctx->variables[ctx->count].name = malloc(strlen(var_name) + 1);
    strcpy(ctx->variables[ctx->count].name, var_name);
    ctx->variables[ctx->count].type = TYPE_STRUCT;
    ctx->variables[ctx->count].struct_type = struct_type;
    ctx->variables[ctx->count].element_type = TYPE_UNKNOWN;
    ctx->variables[ctx->count].pointer_target_type = TYPE_UNKNOWN;
    ctx->variables[ctx->count].array_length = -1;
    ctx->count++;
}
StructTypeInfo* create_struct_type(const char* name) {
    if (!name) return NULL;
    
    StructTypeInfo* struct_type = malloc(sizeof(StructTypeInfo));
    struct_type->name = malloc(strlen(name) + 1);
    strcpy(struct_type->name, name);
    struct_type->fields = NULL;
    struct_type->field_count = 0;
    struct_type->total_size = 0;
    
    return struct_type;
}
void free_struct_type(StructTypeInfo* struct_type) {
    if (!struct_type) return;
    
    free(struct_type->name);
    
    for (int i = 0; i < struct_type->field_count; i++) {
        free(struct_type->fields[i].name);
    }
    free(struct_type->fields);
    free(struct_type);
}
StructTypeInfo* get_struct_type(TypeInferenceContext* ctx, const char* struct_name) {
    if (!ctx || !struct_name) return NULL;
    
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->variables[i].type == TYPE_STRUCT && 
            ctx->variables[i].struct_type && 
            strcmp(ctx->variables[i].struct_type->name, struct_name) == 0) {
            return ctx->variables[i].struct_type;
        }
    }
    
    return NULL;
}

InferredType get_struct_field_type(TypeInferenceContext* ctx, const char* struct_name, const char* field_name) {
    StructTypeInfo* struct_type = get_struct_type(ctx, struct_name);
    if (!struct_type) return TYPE_UNKNOWN;
    
    for (int i = 0; i < struct_type->field_count; i++) {
        if (strcmp(struct_type->fields[i].name, field_name) == 0) {
            return struct_type->fields[i].type;
        }
    }
    
    return TYPE_UNKNOWN;
}

const char* type_to_cpp_string(InferredType type) {
    switch (type) {
        case TYPE_INT:
            return "long long";
        case TYPE_FLOAT:
            return "float";
        case TYPE_STRING:
            return "VString";
        case TYPE_LIST:
            return "VList";
        case TYPE_POINTER:
            return "void*";
        case TYPE_STRUCT:
            return "struct";
        case TYPE_INT8:
            return "char";
        default:
            return "auto"; 
    }
}
static InferredType infer_index_type(TypeInferenceContext* ctx, ASTNode* node) {
    if (!node) return TYPE_UNKNOWN;
    ASTNode* target = node->data.index.target;
    if (!target) return TYPE_UNKNOWN;
    if (target->type == AST_IDENTIFIER && node->data.index.index->type == AST_IDENTIFIER) {
        const char* obj_name = target->data.identifier.name;
        const char* field_name = node->data.index.index->data.identifier.name;
        InferredType obj_type = get_variable_type(ctx, obj_name);
        if (obj_type == TYPE_STRUCT) {
            for (int i = 0; i < ctx->count; i++) {
                if (strcmp(ctx->variables[i].name, obj_name) == 0 && 
                    ctx->variables[i].type == TYPE_STRUCT && 
                    ctx->variables[i].struct_type) {
                    for (int j = 0; j < ctx->variables[i].struct_type->field_count; j++) {
                        if (strcmp(ctx->variables[i].struct_type->fields[j].name, field_name) == 0) {
                            if (ctx->variables[i].struct_type->fields[j].type == TYPE_STRUCT) {
                                return TYPE_STRUCT;
                            }
                            return ctx->variables[i].struct_type->fields[j].type;
                        }
                    }
                    break;
                }
            }
        }
    }
    else if (target->type == AST_INDEX && node->data.index.index->type == AST_IDENTIFIER) {
        return get_nested_field_type(ctx, node);
    }
    
    if (target->type == AST_EXPRESSION_LIST) {
        if (target->data.expression_list.expression_count == 0) return TYPE_UNKNOWN;
        return infer_type(ctx, target->data.expression_list.expressions[0]);
    }
    if (target->type == AST_IDENTIFIER) {
        const char* name = target->data.identifier.name;
        for (int i = 0; i < ctx->count; i++) {
            if (strcmp(ctx->variables[i].name, name) == 0) {
                if (ctx->variables[i].type == TYPE_LIST) return ctx->variables[i].element_type;
                return TYPE_UNKNOWN;
            }
        }
    }
    InferredType t = infer_type(ctx, target);
    if (t == TYPE_LIST) return TYPE_UNKNOWN;
    return TYPE_UNKNOWN;
}

InferredType infer_type_from_value(const char* value) {
    if (!value) return TYPE_UNKNOWN;
    if (strlen(value) == 0) {
        return TYPE_STRING;
    }
    
    int has_quote = 0;
    int has_non_digit = 0;
    int has_dot = 0;
    
    for (int i = 0; value[i] != '\0'; i++) {
        if (value[i] == '"' || value[i] == '\'') {
            has_quote = 1;
        } else if (value[i] == '.') {
            has_dot = 1;
        } else if (!isdigit(value[i]) && value[i] != '-' && value[i] != '+') {
            has_non_digit = 1;
        }
    }
    
    if (has_quote) {
        return TYPE_STRING;
    }
    
    if (has_non_digit) {
        return TYPE_STRING;
    }
    
    if (has_dot) {
        return TYPE_FLOAT;
    }
    
    return TYPE_INT;
}

int has_variable(TypeInferenceContext* ctx, const char* var_name) {
    if (!ctx || !var_name) return 0;
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->variables[i].name, var_name) == 0) return 1;
    }
    return 0;
}
void process_struct_definition(TypeInferenceContext* ctx, ASTNode* struct_def_node) {
    if (!struct_def_node || struct_def_node->type != AST_STRUCT_DEF) return;
    
    const char* struct_name = struct_def_node->data.struct_def.name;
    ASTNode* fields_node = struct_def_node->data.struct_def.fields;
    StructTypeInfo* struct_type = create_struct_type(struct_name);
    if (fields_node && fields_node->type == AST_EXPRESSION_LIST) {
        for (int i = 0; i < fields_node->data.expression_list.expression_count; i++) {
            ASTNode* field = fields_node->data.expression_list.expressions[i];
            if (field && field->type == AST_ASSIGN && 
                field->data.assign.left && field->data.assign.left->type == AST_IDENTIFIER) {
                const char* field_name = field->data.assign.left->data.identifier.name;
                InferredType field_type = TYPE_UNKNOWN;
                if (field->data.assign.right) {
                    field_type = infer_type(ctx, field->data.assign.right);
                }
                struct_type->fields = checked_realloc(struct_type->fields, 
                    sizeof(StructField) * (struct_type->field_count + 1));
                struct_type->fields[struct_type->field_count].name = malloc(strlen(field_name) + 1);
                strcpy(struct_type->fields[struct_type->field_count].name, field_name);
                struct_type->fields[struct_type->field_count].type = field_type;
                struct_type->fields[struct_type->field_count].offset = -1; // 表示尚未计算
                struct_type->fields[struct_type->field_count].struct_type = NULL;
                
                struct_type->field_count++;
            }
        }
    }
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->variables[i].name, struct_name) == 0) {
            if (ctx->variables[i].struct_type) {
                free_struct_type(ctx->variables[i].struct_type);
            }
            ctx->variables[i].struct_type = struct_type;
            ctx->variables[i].type = TYPE_STRUCT; // 标记这是一个结构体类型定义
            return;
        }
    }
    if (ctx->count >= ctx->capacity) {
        ctx->capacity = ctx->capacity == 0 ? 10 : ctx->capacity * 2;
        ctx->variables = checked_realloc(ctx->variables, sizeof(VariableInfo) * ctx->capacity);
    }
    
    ctx->variables[ctx->count].name = malloc(strlen(struct_name) + 1);
    strcpy(ctx->variables[ctx->count].name, struct_name);
    ctx->variables[ctx->count].type = TYPE_STRUCT;
    ctx->variables[ctx->count].struct_type = struct_type;
    ctx->variables[ctx->count].element_type = TYPE_UNKNOWN;
    ctx->variables[ctx->count].pointer_target_type = TYPE_UNKNOWN;
    ctx->count++;
}
