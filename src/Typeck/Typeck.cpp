/*
 * Copyright (c) 2026 Vixlang. All rights reserved.
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
//#include "../../include/typeck.h"

#include <cstring>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../include/ast.h"
#include "../../include/compiler.h"
#include "../../include/env.h"
#include "../../include/parser.h"
#include "../../include/type.h"
#include "../../include/unify.h"

extern "C" const char* current_input_filename;

namespace {

const char* node_file(const ASTNode* node) {
	if (node && node->source_file) {
		return node->source_file;
	}
	return current_input_filename ? current_input_filename : "unknown";
}

int node_line(const ASTNode* node) {
	if (node && node->location.first_line > 0) {
		return node->location.first_line;
	}
	return 1;
}

int node_col(const ASTNode* node) {
	if (node && node->location.first_column > 0) {
		return node->location.first_column;
	}
	return 1;
}

struct TypeChecker {
	TypeEnv env;
	Unifier unify;
	std::unordered_map<const ASTNode*, TypePtr> node_types;
	std::unordered_map<std::string, TypePtr> match_payloads;
	std::unordered_map<std::string, TypePtr> match_payload_field_types;
	std::unordered_map<std::string, TypePtr> generic_bindings;
	std::set<std::string> making_type_info_for;
	int error_count = 0;

	TypePtr builtin_void = Type::make(TypeKind::Void);
	TypePtr builtin_i8 = Type::make(TypeKind::I8);
	TypePtr builtin_i32 = Type::make(TypeKind::I32);
	TypePtr builtin_i64 = Type::make(TypeKind::I64);
	TypePtr builtin_f32 = Type::make(TypeKind::F32);
	TypePtr builtin_f64 = Type::make(TypeKind::F64);
	TypePtr builtin_bool = Type::make(TypeKind::Bool);
	TypePtr builtin_string = Type::make(TypeKind::String);

	bool is_builtin_ctor(const char* name) const {
		if (!name) {
			return false;
		}
		return strcmp(name, "Some") == 0 || strcmp(name, "None") == 0 ||
			   strcmp(name, "Ok") == 0 || strcmp(name, "Err") == 0;
	}

	bool is_registered_ctor(const char* name) const {
		return name && vix_adt_ctor_base_name(name) != nullptr;
	}

	void register_builtin_ctors() {
		if (!env.lookup_ctor("Some")) {
			TypePtr tvar = unify.fresh();
			TypePtr opt = Type::make_app(Type::make_struct("Option"), {tvar});
			env.register_ctor("Some", Type::make_fn({tvar}, opt));
		}
		if (!env.lookup_ctor("None")) {
			TypePtr opt = Type::make_app(Type::make_struct("Option"), {unify.fresh()});
			env.register_ctor("None", opt);
		}
		if (!env.lookup_ctor("Ok")) {
			TypePtr tvar = unify.fresh();
			TypePtr res = Type::make_app(Type::make_struct("Result"), {tvar, unify.fresh()});
			env.register_ctor("Ok", Type::make_fn({tvar}, res));
		}
		if (!env.lookup_ctor("Err")) {
			TypePtr tvar = unify.fresh();
			TypePtr res = Type::make_app(Type::make_struct("Result"), {unify.fresh(), tvar});
			env.register_ctor("Err", Type::make_fn({tvar}, res));
		}
	}

	void report_type_error(const ASTNode* node, const std::string& message) {
		const char* filename = node_file(node);
		int line = node_line(node);
		int col = node_col(node);
		set_location_with_column(filename, line, col);
		report_simple_error(ERROR_LEVEL_ERROR, ERROR_TYPE, message.c_str());
		error_count++;
	}

	void report_semantic_error(const ASTNode* node, const std::string& message) {
		const char* filename = node_file(node);
		int line = node_line(node);
		int col = node_col(node);
		set_location_with_column(filename, line, col);
		report_simple_error(ERROR_LEVEL_ERROR, ERROR_SEMANTIC, message.c_str());
		error_count++;
	}

	int node_span_length(const ASTNode* node, int fallback) {
		if (node && node->location.first_line == node->location.last_line &&
			node->location.last_column > node->location.first_column) {
			return node->location.last_column - node->location.first_column;
		}
		return fallback > 0 ? fallback : 1;
	}

	void report_warning_at(const ASTNode* node, const std::string& message, int length = 1) {
		const char* filename = node_file(node);
		int line = node_line(node);
		int col = node_col(node);
		set_location_with_column(filename, line, col);
		report_simple_error_with_length(ERROR_LEVEL_WARNING, ERROR_WARNING, message.c_str(), length);
	}

	void report_warning_at_snippet(const ASTNode* node, const std::string& message, const std::string& snippet) {
		report_warning_with_location_and_snippet(message.c_str(), node_file(node), node_line(node), snippet.c_str());
	}

	struct ArrayParamUsage {
		std::string name;
		ASTNode* declaration = nullptr;
		ASTNode* first_modify = nullptr;
		int first_modify_length = 1;
		std::string first_modify_snippet;
		ASTNode* first_length_read = nullptr;
		int first_length_read_length = 1;
		std::string first_length_read_snippet;
		ASTNode* first_length_read_after_modify = nullptr;
		bool used = false;
		bool modified = false;
		bool returned = false;
		bool warned_modify_after_length = false;
	};

	bool is_array_like_type(const TypePtr& type) {
		if (!type) {
			return false;
		}
		TypePtr resolved = unify.apply(type);
		return resolved->kind == TypeKind::Array || resolved->kind == TypeKind::FixedArray;
	}

	ArrayParamUsage* find_array_param(std::vector<ArrayParamUsage>& params, const char* name) {
		if (!name) {
			return nullptr;
		}
		for (auto& param : params) {
			if (param.name == name) {
				return &param;
			}
		}
		return nullptr;
	}

	const char* identifier_name(ASTNode* node) {
		return node && node->type == AST_IDENTIFIER ? node->data.identifier.name : nullptr;
	}

	int member_expr_length(const char* object_name, const char* field_name) {
		int object_len = object_name ? (int)strlen(object_name) : 1;
		int field_len = field_name ? (int)strlen(field_name) : 0;
		return object_len + (field_len > 0 ? field_len + 1 : 0);
	}

	std::string member_expr_snippet(const char* object_name, const char* field_name) {
		std::string snippet = object_name ? object_name : "";
		if (field_name && *field_name) {
			snippet += ".";
			snippet += field_name;
		}
		return snippet;
	}

	void scan_array_param_usage(ASTNode* node, std::vector<ArrayParamUsage>& params, bool in_return) {
		if (!node) {
			return;
		}

		switch (node->type) {
			case AST_IDENTIFIER: {
				if (ArrayParamUsage* usage = find_array_param(params, node->data.identifier.name)) {
					usage->used = true;
					if (in_return) {
						usage->returned = true;
					}
				}
				break;
			}
			case AST_RETURN:
				scan_array_param_usage(node->data.return_stmt.expr, params, true);
				break;
			case AST_CALL: {
				ASTNode* func = node->data.call.func;
				bool handled_push_receiver = false;
				if (func && func->type == AST_MEMBER_ACCESS) {
					ASTNode* object = func->data.member_access.object;
					const char* object_name = identifier_name(object);
					const char* field_name = identifier_name(func->data.member_access.field);
					if (ArrayParamUsage* usage = find_array_param(params, object_name)) {
						usage->used = true;
						if (field_name && strcmp(field_name, "push") == 0) {
							usage->modified = true;
							if (!usage->first_modify) {
								usage->first_modify = object ? object : node;
								usage->first_modify_length = member_expr_length(object_name, field_name);
								usage->first_modify_snippet = member_expr_snippet(object_name, field_name);
							}
							if (usage->first_length_read && !usage->warned_modify_after_length) {
					report_warning_at_snippet(usage->first_length_read,
						"array parameter '" + usage->name +
						"' is modified after reading its length\n"
						"  note: the length was read at line " + std::to_string(node_line(usage->first_length_read)) +
						", but modifications to the copy won't affect the original",
						usage->first_length_read_snippet);
								usage->warned_modify_after_length = true;
							}
							handled_push_receiver = true;
						}
					}
				}
				if (!handled_push_receiver) {
					scan_array_param_usage(func, params, in_return);
				}
				if (node->data.call.args && node->data.call.args->type == AST_EXPRESSION_LIST) {
					int count = node->data.call.args->data.expression_list.expression_count;
					for (int i = 0; i < count; i++) {
						scan_array_param_usage(node->data.call.args->data.expression_list.expressions[i], params, in_return);
					}
				}
				break;
			}
			case AST_MEMBER_ACCESS: {
				ASTNode* object = node->data.member_access.object;
				const char* object_name = identifier_name(object);
				const char* field_name = identifier_name(node->data.member_access.field);
				if (ArrayParamUsage* usage = find_array_param(params, object_name)) {
					usage->used = true;
					if (field_name && (strcmp(field_name, "length") == 0 || strcmp(field_name, "size") == 0)) {
						if (!usage->first_length_read) {
							usage->first_length_read = object ? object : node;
							usage->first_length_read_length = member_expr_length(object_name, field_name);
							usage->first_length_read_snippet = member_expr_snippet(object_name, field_name);
						}
						if (usage->modified && !usage->first_length_read_after_modify) {
							usage->first_length_read_after_modify = node;
						}
					}
				} else {
					scan_array_param_usage(object, params, in_return);
				}
				break;
			}
			case AST_PROGRAM:
				for (int i = 0; i < node->data.program.statement_count; i++) {
					scan_array_param_usage(node->data.program.statements[i], params, in_return);
				}
				break;
			case AST_ASSIGN:
			case AST_CONST:
				scan_array_param_usage(node->data.assign.left, params, in_return);
				scan_array_param_usage(node->data.assign.right, params, in_return);
				break;
			case AST_BINOP:
				scan_array_param_usage(node->data.binop.left, params, in_return);
				scan_array_param_usage(node->data.binop.right, params, in_return);
				break;
			case AST_UNARYOP:
				scan_array_param_usage(node->data.unaryop.expr, params, in_return);
				break;
			case AST_IF:
				scan_array_param_usage(node->data.if_stmt.condition, params, in_return);
				scan_array_param_usage(node->data.if_stmt.then_body, params, in_return);
				scan_array_param_usage(node->data.if_stmt.else_body, params, in_return);
				break;
			case AST_WHILE:
				scan_array_param_usage(node->data.while_stmt.condition, params, in_return);
				scan_array_param_usage(node->data.while_stmt.body, params, in_return);
				break;
			case AST_FOR:
				scan_array_param_usage(node->data.for_stmt.start, params, in_return);
				scan_array_param_usage(node->data.for_stmt.end, params, in_return);
				scan_array_param_usage(node->data.for_stmt.body, params, in_return);
				break;
			case AST_INDEX:
				scan_array_param_usage(node->data.index.target, params, in_return);
				scan_array_param_usage(node->data.index.index, params, in_return);
				break;
			case AST_STRUCT_LITERAL:
				scan_array_param_usage(node->data.struct_literal.fields, params, in_return);
				break;
			case AST_EXPRESSION_LIST:
				for (int i = 0; i < node->data.expression_list.expression_count; i++) {
					scan_array_param_usage(node->data.expression_list.expressions[i], params, in_return);
				}
				break;
			case AST_PRINT:
				scan_array_param_usage(node->data.print.expr, params, in_return);
				break;
			case AST_TOINT:
				scan_array_param_usage(node->data.toint.expr, params, in_return);
				break;
			case AST_TOFLOAT:
				scan_array_param_usage(node->data.tofloat.expr, params, in_return);
				break;
			default:
				break;
		}
	}

	std::vector<ArrayParamUsage> collect_array_params(ASTNode* fn) {
		std::vector<ArrayParamUsage> params;
		if (!fn || !fn->data.function.params || fn->data.function.params->type != AST_EXPRESSION_LIST) {
			return params;
		}
		int count = fn->data.function.params->data.expression_list.expression_count;
		for (int i = 0; i < count; i++) {
			ASTNode* param = fn->data.function.params->data.expression_list.expressions[i];
			if (!param || param->type != AST_ASSIGN || !param->data.assign.left ||
				param->data.assign.left->type != AST_IDENTIFIER) {
				continue;
			}
			TypePtr ptype = type_from_ast(param->data.assign.right);
			if (!is_array_like_type(ptype)) {
				continue;
			}
			ArrayParamUsage usage;
			usage.name = param->data.assign.left->data.identifier.name ? param->data.assign.left->data.identifier.name : "";
			usage.declaration = param->data.assign.left;
			if (!usage.name.empty()) {
				params.push_back(std::move(usage));
			}
		}
		return params;
	}

	void warn_array_parameter_value_semantics(ASTNode* fn) {
		std::vector<ArrayParamUsage> params = collect_array_params(fn);
		if (params.empty()) {
			return;
		}
		scan_array_param_usage(fn->data.function.body, params, false);
		for (const auto& usage : params) {
			if (!usage.used) {
				report_warning_at(usage.declaration,
					"array parameter '" + usage.name + "' is never used\n"
					"  note: array parameters are passed by value (copy)");
				continue;
			}
			if (usage.modified && !usage.returned && usage.first_modify) {
				report_warning_at_snippet(usage.first_modify,
					"array parameter '" + usage.name + "' is modified but changes are lost\n"
					"  note: array parameters are passed by value (copy)",
					usage.first_modify_snippet);
			}
			if (usage.first_length_read_after_modify && !usage.returned) {
				report_warning_at(usage.first_length_read_after_modify,
					"reading array parameter '" + usage.name + "' after modifying a copy\n"
					"  note: the array parameter was modified earlier, but the original caller's array is unchanged");
			}
		}
	}

	ASTNode* find_return_node(ASTNode* node) {
		if (!node) return nullptr;
		if (node->type == AST_RETURN) return node;
		if (node->type == AST_PROGRAM) {
			for (int i = 0; i < node->data.program.statement_count; i++) {
				ASTNode* found = find_return_node(node->data.program.statements[i]);
				if (found) return found;
			}
		}
		if (node->type == AST_IF) {
			ASTNode* found = find_return_node(node->data.if_stmt.then_body);
			if (found) return found;
			found = find_return_node(node->data.if_stmt.else_body);
			if (found) return found;
		}
		if (node->type == AST_WHILE) {
			return find_return_node(node->data.while_stmt.body);
		}
		if (node->type == AST_FOR) {
			return find_return_node(node->data.for_stmt.body);
		}
		return nullptr;
	}

	TypeInfo* alloc_type_info(TypeInfoKind kind) {
		TypeInfo* info = (TypeInfo*)calloc(1, sizeof(TypeInfo));
		if (!info) {
			return nullptr;
		}
		info->kind = kind;
		return info;
	}

	TypeInfo* make_type_info(const TypePtr& type) {
		if (!type) {
			return alloc_type_info(TYPEINFO_VOID);
		}
		TypePtr resolved = unify.apply(type);
		switch (resolved->kind) {
			case TypeKind::Void:
				return alloc_type_info(TYPEINFO_VOID);
			case TypeKind::I8:
				return alloc_type_info(TYPEINFO_I8);
			case TypeKind::I32:
				return alloc_type_info(TYPEINFO_I32);
			case TypeKind::I64:
				return alloc_type_info(TYPEINFO_I64);
			case TypeKind::F32:
				return alloc_type_info(TYPEINFO_F32);
			case TypeKind::F64:
				return alloc_type_info(TYPEINFO_F64);
			case TypeKind::Bool:
				return alloc_type_info(TYPEINFO_BOOL);
			case TypeKind::String:
				return alloc_type_info(TYPEINFO_STRING);
			case TypeKind::Ptr: {
				TypeInfo* info = alloc_type_info(TYPEINFO_PTR);
				if (!info) return nullptr;
				TypePtr pointee = resolved->data.ptr.pointee;
				if (pointee && pointee->kind == TypeKind::Struct) {
					std::string sname = pointee->data.struct_data.name;
					if (!sname.empty() && making_type_info_for.count(sname)) {
						info->element = alloc_type_info(TYPEINFO_STRUCT);
						if (info->element) {
							info->element->name = strdup(sname.c_str());
						}
						return info;
					}
				}
				info->element = make_type_info(pointee);
				return info;
			}
			case TypeKind::Struct: {
				TypeInfo* info = alloc_type_info(TYPEINFO_STRUCT);
				if (!info) return nullptr;
				const std::string& name = resolved->data.struct_data.name;
				info->name = name.empty() ? nullptr : strdup(name.c_str());
				if (!name.empty() && making_type_info_for.count(name)) {
					return info;
				}
				if (!name.empty()) {
					making_type_info_for.insert(name);
					const StructInfo* si = env.lookup_struct(name);
					if (si && !si->fields.empty()) {
						info->param_count = static_cast<int>(si->fields.size());
						info->params = (TypeInfo**)calloc(info->param_count, sizeof(TypeInfo*));
						if (info->params) {
							for (int i = 0; i < info->param_count; i++) {
								info->params[i] = make_type_info(si->fields[i].type);
							}
						}
					}
					making_type_info_for.erase(name);
				}
				return info;
			}
			case TypeKind::Array: {
				TypeInfo* info = alloc_type_info(TYPEINFO_ARRAY);
				if (!info) return nullptr;
				info->element = make_type_info(resolved->data.array.element);
				return info;
			}
			case TypeKind::FixedArray: {
				TypeInfo* info = alloc_type_info(TYPEINFO_FIXED_ARRAY);
				if (!info) return nullptr;
				info->element = make_type_info(resolved->data.fixed_array.element);
				info->size = resolved->data.fixed_array.size;
				return info;
			}
			case TypeKind::Fn: {
				TypeInfo* info = alloc_type_info(TYPEINFO_FN);
				if (!info) return nullptr;
				info->param_count = static_cast<int>(resolved->data.fn.params.size());
				if (info->param_count > 0) {
					info->params = (TypeInfo**)calloc(info->param_count, sizeof(TypeInfo*));
					for (int i = 0; i < info->param_count; i++) {
						info->params[i] = make_type_info(resolved->data.fn.params[i]);
					}
				}
				info->ret = make_type_info(resolved->data.fn.ret);
				return info;
			}
			case TypeKind::App: {
				TypeInfo* info = alloc_type_info(TYPEINFO_APP);
				if (!info) return nullptr;
				info->app_ctor = make_type_info(resolved->data.app.ctor);
				info->app_arg_count = static_cast<int>(resolved->data.app.args.size());
				if (info->app_arg_count > 0) {
					info->app_args = (TypeInfo**)calloc(info->app_arg_count, sizeof(TypeInfo*));
					for (int i = 0; i < info->app_arg_count; i++) {
						info->app_args[i] = make_type_info(resolved->data.app.args[i]);
					}
				}
				return info;
			}
			case TypeKind::Var:
			default:
				return alloc_type_info(TYPEINFO_VAR);
		}
	}

	void store_inferred_type(ASTNode* node, const TypePtr& type) {
		if (!node) return;
		if (node->inferred_type) {
			free_type_info(node->inferred_type);
			node->inferred_type = nullptr;
		}
		node->inferred_type = make_type_info(type);
	}

	TypePtr record_node_type(ASTNode* node, const TypePtr& type) {
		TypePtr resolved = type ? unify.apply(type) : builtin_void;
		if (node) {
			node_types[node] = resolved;
			store_inferred_type(node, resolved);
		}
		return resolved;
	}

	TypePtr type_from_ast(ASTNode* type_node) {
		if (!type_node) {
			return builtin_void;
		}

		switch (type_node->type) {
			case AST_TYPE_INT8:
				return builtin_i8;
			case AST_TYPE_INT32:
				return builtin_i32;
			case AST_TYPE_INT64:
				return builtin_i64;
			case AST_TYPE_FLOAT32:
				return builtin_f32;
			case AST_TYPE_FLOAT64:
				return builtin_f64;
			case AST_TYPE_STRING:
				return builtin_string;
			case AST_TYPE_VOID:
				return builtin_void;
			case AST_TYPE_POINTER:
					if (type_node->data.pointer_type.element_type) {
						return Type::make_ptr(type_from_ast(type_node->data.pointer_type.element_type));
					}
					return Type::make_ptr(unify.fresh());
			case AST_TYPE_LIST:
					return Type::make_array(type_node->data.list_type.element_type
										? type_from_ast(type_node->data.list_type.element_type)
										: unify.fresh());
			case AST_TYPE_FIXED_SIZE_LIST:
				return Type::make_fixed_array(type_from_ast(type_node->data.fixed_size_list_type.element_type),
											  static_cast<size_t>(type_node->data.fixed_size_list_type.size));
			case AST_TYPE_APP: {
				TypePtr ctor = type_from_ast(type_node->data.type_app.ctor);
				std::vector<TypePtr> args;
				if (type_node->data.type_app.args && type_node->data.type_app.args->type == AST_EXPRESSION_LIST) {
					int count = type_node->data.type_app.args->data.expression_list.expression_count;
					args.reserve(count);
					for (int i = 0; i < count; i++) {
						args.push_back(type_from_ast(type_node->data.type_app.args->data.expression_list.expressions[i]));
					}
				}
				if (ctor->kind == TypeKind::Struct && ctor->data.struct_data.name == "Fn" && !args.empty()) {
					TypePtr ret = args.back();
					args.pop_back();
					return Type::make_fn(std::move(args), std::move(ret));
				}
				return Type::make_app(ctor, std::move(args));
			}
		case AST_IDENTIFIER:
				if (type_node->data.identifier.name) {
					const char* name = type_node->data.identifier.name;
					if (strcmp(name, "ptr") == 0) {
						return Type::make_ptr(unify.fresh());
					}
					if (strcmp(name, "bool") == 0) {
						return builtin_bool;
					}
					if (strcmp(name, "i8") == 0) {
						return builtin_i8;
					}
					if (strcmp(name, "i32") == 0) {
						return builtin_i32;
					}
					if (strcmp(name, "i64") == 0) {
						return builtin_i64;
					}
					if (strcmp(name, "f32") == 0) {
						return builtin_f32;
					}
					if (strcmp(name, "f64") == 0) {
						return builtin_f64;
					}
					if (strcmp(name, "string") == 0) {
						return builtin_string;
					}
					if (strcmp(name, "void") == 0) {
						return builtin_void;
					}
					auto it = generic_bindings.find(name);
					if (it != generic_bindings.end()) {
						return it->second;
					}
				}
				return Type::make_struct(type_node->data.identifier.name ? type_node->data.identifier.name : "<anon>");
			case AST_EXPRESSION_LIST: {
				int cnt = type_node->data.expression_list.expression_count;
				if (cnt == 1) {
					return type_from_ast(type_node->data.expression_list.expressions[0]);
				}
				std::vector<TypePtr> elems;
				elems.reserve(cnt);
				for (int i = 0; i < cnt; i++) {
					elems.push_back(type_from_ast(type_node->data.expression_list.expressions[i]));
				}
				return Type::make_tuple(std::move(elems));
			}
			default:
				return unify.fresh();
		}
	}

	bool is_numeric(const TypePtr& type) {
		if (!type) {
			return false;
		}
		TypePtr t = unify.apply(type);
		return t->kind == TypeKind::I8 || t->kind == TypeKind::I32 || t->kind == TypeKind::I64 ||
			   t->kind == TypeKind::F32 || t->kind == TypeKind::F64;
	}

	bool is_valid_vararg_type(const TypePtr& type) {
		if (!type) {
			return false;
		}
		TypePtr t = unify.apply(type);
		return t->kind == TypeKind::I8 || t->kind == TypeKind::I32 || t->kind == TypeKind::I64 ||
			   t->kind == TypeKind::F32 || t->kind == TypeKind::F64 || t->kind == TypeKind::Bool ||
			   t->kind == TypeKind::String || t->kind == TypeKind::Ptr || t->kind == TypeKind::Var;
	}
	TypePtr freshen_type(const TypePtr& t) {
		if (!t) return t;
		TypePtr a = unify.apply(t);
		if (a->kind == TypeKind::Var) {
			return unify.fresh();
		}
		switch (a->kind) {
			case TypeKind::Ptr:
				return Type::make_ptr(freshen_type(a->data.ptr.pointee));
			case TypeKind::Array:
				return Type::make_array(freshen_type(a->data.array.element));
			case TypeKind::FixedArray:
				return Type::make_fixed_array(freshen_type(a->data.fixed_array.element),
											  a->data.fixed_array.size);
			case TypeKind::App: {
				std::vector<TypePtr> args;
				args.reserve(a->data.app.args.size());
				for (const auto& arg : a->data.app.args) {
					args.push_back(freshen_type(arg));
				}
				return Type::make_app(freshen_type(a->data.app.ctor), std::move(args));
			}
			case TypeKind::Fn: {
				std::vector<TypePtr> params;
				params.reserve(a->data.fn.params.size());
				for (const auto& p : a->data.fn.params) {
					params.push_back(freshen_type(p));
				}
				return Type::make_fn(std::move(params), freshen_type(a->data.fn.ret));
			}
			case TypeKind::Tuple: {
				std::vector<TypePtr> elems;
				elems.reserve(a->data.tuple.elements.size());
				for (const auto& e : a->data.tuple.elements) {
					elems.push_back(freshen_type(e));
				}
				return Type::make_tuple(std::move(elems));
			}
			default:
				return a;
		}
	}

	int numeric_rank(const TypePtr& type) {
		TypePtr t = unify.apply(type);
		switch (t->kind) {
			case TypeKind::I8:
				return 0;
			case TypeKind::I32:
				return 1;
			case TypeKind::I64:
				return 2;
			case TypeKind::F32:
				return 3;
			case TypeKind::F64:
				return 4;
			default:
				return -1;
		}
	}

	TypePtr promote_numeric(const TypePtr& lhs, const TypePtr& rhs) {
		if (!is_numeric(lhs) || !is_numeric(rhs)) {
			return unify.fresh();
		}
		int lr = numeric_rank(lhs);
		int rr = numeric_rank(rhs);
		if (lr >= rr) {
			return unify.apply(lhs);
		}
		return unify.apply(rhs);
	}

	bool is_string_compatible(const TypePtr& type) {
		if (!type) {
			return false;
		}
		TypePtr t = unify.apply(type);
		return t->kind == TypeKind::String || t->kind == TypeKind::Ptr || t->kind == TypeKind::Var;
	}

	TypePtr register_function(ASTNode* fn) {
		std::vector<TypePtr> params;
		std::vector<int> generic_ids;
		auto saved_generics = generic_bindings;
		generic_bindings.clear();

		if (fn->data.function.generic_params && fn->data.function.generic_params->type == AST_EXPRESSION_LIST) {
			int cnt = fn->data.function.generic_params->data.expression_list.expression_count;
			for (int i = 0; i < cnt; i++) {
				ASTNode* g = fn->data.function.generic_params->data.expression_list.expressions[i];
				TypePtr gvar = unify.fresh();
				generic_ids.push_back(gvar->data.var.id);
				if (g && g->type == AST_IDENTIFIER && g->data.identifier.name) {
					generic_bindings[g->data.identifier.name] = gvar;
				}
			}
		}

		if (fn->data.function.params && fn->data.function.params->type == AST_EXPRESSION_LIST) {
			int count = fn->data.function.params->data.expression_list.expression_count;
			for (int i = 0; i < count; i++) {
				ASTNode* param = fn->data.function.params->data.expression_list.expressions[i];
				if (!param) {
					params.push_back(unify.fresh());
					continue;
				}
				if (param->type == AST_ASSIGN && param->data.assign.right) {
					params.push_back(type_from_ast(param->data.assign.right));
				} else {
					params.push_back(unify.fresh());
				}
			}
		}

		TypePtr ret = type_from_ast(fn->data.function.return_type);
		TypePtr fn_type = Type::make_fn(std::move(params), ret, generic_ids, fn->data.function.vararg != 0);

		env.declare_value(fn->data.function.name, fn_type, false, true);
		generic_bindings = std::move(saved_generics);
		return fn_type;
	}

	void ensure_bool(ASTNode* node, const TypePtr& t) {
		TypePtr resolved = unify.apply(t);
		if (resolved->kind == TypeKind::Bool) return;
		if (is_numeric(resolved)) return;
		if (resolved->kind == TypeKind::Var) {
			try {
				unify.unify(t, builtin_bool);
			} catch (...) {}
			return;
		}
		try {
			unify.unify(t, builtin_bool);
		} catch (const std::exception& ex) {
			report_type_error(node, ex.what());
		}
	}

	TypePtr check_program(ASTNode* node) {
		if (!node || node->type != AST_PROGRAM) {
			return builtin_void;
		}

		register_builtin_ctors();

		// Predeclare functions.
		for (int i = 0; i < node->data.program.statement_count; i++) {
			ASTNode* stmt = node->data.program.statements[i];
			if (stmt && stmt->type == AST_FUNCTION) {
				register_function(stmt);
			}
			// Also pre-declare functions from extern blocks (nested PROGRAM nodes).
			if (stmt && stmt->type == AST_PROGRAM) {
				for (int j = 0; j < stmt->data.program.statement_count; j++) {
					ASTNode* inner = stmt->data.program.statements[j];
					if (inner && inner->type == AST_FUNCTION) {
						register_function(inner);
					}
				}
			}
		}

		// Predeclare structs.
		for (int i = 0; i < node->data.program.statement_count; i++) {
			ASTNode* stmt = node->data.program.statements[i];
			if (stmt && stmt->type == AST_STRUCT_DEF) {
				check_struct_def(stmt);
			}
		}

		// Process ADT definitions (desugared to const declarations).
		for (int i = 0; i < node->data.program.statement_count; i++) {
			ASTNode* stmt = node->data.program.statements[i];
			check_adt_def(stmt);
		}

		TypePtr last = builtin_void;
		for (int i = 0; i < node->data.program.statement_count; i++) {
			ASTNode* stmt = node->data.program.statements[i];
			// Flatten extern blocks: process their inner statements without a new scope
			// so that extern function declarations are visible in the outer scope.
			if (stmt && stmt->type == AST_PROGRAM) {
				bool all_extern = true;
				for (int j = 0; j < stmt->data.program.statement_count; j++) {
					ASTNode* inner = stmt->data.program.statements[j];
					if (!inner || inner->type != AST_FUNCTION || !inner->data.function.is_extern) {
						all_extern = false;
						break;
					}
				}
				if (all_extern && stmt->data.program.statement_count > 0) {
					for (int j = 0; j < stmt->data.program.statement_count; j++) {
						check_expr(stmt->data.program.statements[j]);
					}
					continue;
				}
			}
			last = check_expr(stmt);
		}
		return last;
	}

	TypePtr check_block(ASTNode* node, bool new_scope) {
		if (!node) {
			return builtin_void;
		}
		if (new_scope) {
			env.enter_scope();
		}
		TypePtr last = builtin_void;
		if (node->type == AST_PROGRAM) {
			for (int i = 0; i < node->data.program.statement_count; i++) {
				last = check_expr(node->data.program.statements[i]);
			}
		} else {
			last = check_expr(node);
		}
		if (new_scope) {
			env.exit_scope();
		}
		return last;
	}

	bool type_references_struct(const TypePtr& t, const std::string& sname) {
		if (!t) return false;
		TypePtr a = unify.apply(t);
		if (a->kind == TypeKind::Struct) {
			return a->data.struct_data.name == sname;
		}
		if (a->kind == TypeKind::App) {
			if (type_references_struct(a->data.app.ctor, sname)) return true;
			for (const auto& arg : a->data.app.args) {
				if (type_references_struct(arg, sname)) return true;
			}
		}
		if (a->kind == TypeKind::Ptr) {
			return false;
		}
		if (a->kind == TypeKind::Array) {
			return type_references_struct(a->data.array.element, sname);
		}
		if (a->kind == TypeKind::Fn) {
			for (const auto& p : a->data.fn.params) {
				if (type_references_struct(p, sname)) return true;
			}
			return type_references_struct(a->data.fn.ret, sname);
		}
		return false;
	}

	void check_struct_def(ASTNode* node) {
		if (!node || node->type != AST_STRUCT_DEF) {
			return;
		}

		auto saved_generics = generic_bindings;
		generic_bindings.clear();
		std::vector<int> struct_generic_ids;
		if (node->data.struct_def.generic_params &&
			node->data.struct_def.generic_params->type == AST_EXPRESSION_LIST) {
			int gcount = node->data.struct_def.generic_params->data.expression_list.expression_count;
			for (int i = 0; i < gcount; i++) {
				ASTNode* g = node->data.struct_def.generic_params->data.expression_list.expressions[i];
				if (!g || g->type != AST_IDENTIFIER || !g->data.identifier.name) {
					continue;
				}
				TypePtr gvar = unify.fresh();
				struct_generic_ids.push_back(gvar->data.var.id);
				generic_bindings[g->data.identifier.name] = gvar;
			}
		}

		StructInfo info;
		info.name = node->data.struct_def.name ? node->data.struct_def.name : "<anon>";
		info.generic_param_ids = struct_generic_ids;
		if (node->data.struct_def.fields && node->data.struct_def.fields->type == AST_EXPRESSION_LIST) {
			int count = node->data.struct_def.fields->data.expression_list.expression_count;
			for (int i = 0; i < count; i++) {
				ASTNode* field = node->data.struct_def.fields->data.expression_list.expressions[i];
				if (!field || field->type != AST_ASSIGN) {
					continue;
				}
				ASTNode* field_name = field->data.assign.left;
				ASTNode* field_type = field->data.assign.right;
				if (!field_name || field_name->type != AST_IDENTIFIER) {
					continue;
				}
				StructFieldInfo finfo;
				finfo.name = field_name->data.identifier.name ? field_name->data.identifier.name : "<field>";
				finfo.type = type_from_ast(field_type);
				finfo.offset = 0;
				info.fields.push_back(std::move(finfo));
			}
		}
		if (!node->data.struct_def.fields || node->data.struct_def.fields->type != AST_EXPRESSION_LIST) {
			generic_bindings = std::move(saved_generics);
			return;
		}
		for (const auto& f : info.fields) {
			if (type_references_struct(f.type, info.name)) {
				report_semantic_error(node, "self-recursive struct fields must use pointer type");
				generic_bindings = std::move(saved_generics);
				return;
			}
		}
		// Check for mutual recursion: collect all non-pointer struct field names
		// and check if any of them reference back to this struct
		{
			std::set<std::string> visited;
			std::function<bool(const std::string&, const std::string&)> has_cycle;
			has_cycle = [&](const std::string& current, const std::string& target) -> bool {
				if (current == target) return true;
				if (visited.count(current)) return false;
				visited.insert(current);
				const StructInfo* si = env.lookup_struct(current);
				if (!si) return false;
				for (const auto& sf : si->fields) {
					TypePtr ft = unify.apply(sf.type);
					if (ft->kind == TypeKind::Struct) {
						if (has_cycle(ft->data.struct_data.name, target)) return true;
					}
				}
				return false;
			};
			for (const auto& f : info.fields) {
				TypePtr ft = unify.apply(f.type);
				if (ft->kind == TypeKind::Struct) {
					visited.clear();
					if (has_cycle(ft->data.struct_data.name, info.name)) {
						report_semantic_error(node, "self-recursive struct fields must use pointer type");
						generic_bindings = std::move(saved_generics);
						return;
					}
				}
			}
		}
		compute_struct_layout(info, env);
		env.register_struct(std::move(info));
		generic_bindings = std::move(saved_generics);
	}

	void check_adt_def(ASTNode* node) {
		if (!node || node->type != AST_CONST) {
			return;
		}
		if (!node->data.assign.left || node->data.assign.left->type != AST_IDENTIFIER) {
			return;
		}
		const char* name = node->data.assign.left->data.identifier.name;
		if (!name) {
			return;
		}
		if (!is_builtin_ctor(name) && !is_registered_ctor(name)) {
			return;
		}

		if (is_registered_ctor(name)) {
			const char* base_name = vix_adt_ctor_base_name(name);
			int arity = base_name ? vix_adt_generic_arity(base_name) : 0;
			int payload_count = vix_adt_ctor_payload_count(name);
			std::vector<TypePtr> args;
			for (int i = 0; i < arity; i++) {
				args.push_back(unify.fresh());
			}
			if (payload_count > 0) {
				ASTNode* payload_type_node = vix_adt_ctor_payload_type_node(name);
				TypePtr payload;
				if (payload_type_node) {
					payload = type_from_ast(payload_type_node);
				}
				if (!payload) {
					payload = unify.fresh();
				}
				TypePtr result = Type::make_app(Type::make_struct(base_name ? base_name : "<anon>"), std::move(args));
				env.register_ctor(name, Type::make_fn({payload}, result));
			} else {
				env.register_ctor(name, Type::make_app(Type::make_struct(base_name ? base_name : "<anon>"), std::move(args)));
			}
			return;
		}

		if (strcmp(name, "None") == 0) {
			TypePtr opt = Type::make_app(Type::make_struct("Option"), {unify.fresh()});
			env.register_ctor("None", opt);
		} else if (strcmp(name, "Some") == 0) {
			TypePtr tvar = unify.fresh();
			TypePtr opt = Type::make_app(Type::make_struct("Option"), {tvar});
			TypePtr ctor = Type::make_fn({tvar}, opt);
			env.register_ctor("Some", ctor);
		} else if (strcmp(name, "Ok") == 0) {
			TypePtr tvar = unify.fresh();
			TypePtr res = Type::make_app(Type::make_struct("Result"), {tvar, unify.fresh()});
			TypePtr ctor = Type::make_fn({tvar}, res);
			env.register_ctor("Ok", ctor);
		} else if (strcmp(name, "Err") == 0) {
			TypePtr tvar = unify.fresh();
			TypePtr res = Type::make_app(Type::make_struct("Result"), {unify.fresh(), tvar});
			TypePtr ctor = Type::make_fn({tvar}, res);
			env.register_ctor("Err", ctor);
		}
	}

	TypePtr check_expr(ASTNode* node) {
		if (!node) {
			return builtin_void;
		}

		TypePtr result = builtin_void;
		switch (node->type) {
			case AST_PROGRAM:
				result = check_block(node, true);
				break;
			case AST_NUM_INT:
				if (node->inferred_type && node->inferred_type->kind == TYPEINFO_BOOL) {
					result = builtin_bool;
				} else {
					result = builtin_i32;
				}
				break;
			case AST_NUM_FLOAT:
				result = builtin_f64;
				break;
			case AST_CHAR:
				result = builtin_i8;
				break;
			case AST_STRING:
				result = builtin_string;
				break;
			case AST_NIL:
				result = Type::make_ptr(unify.fresh());
				break;
			case AST_IDENTIFIER: {
				const char* name = node->data.identifier.name ? node->data.identifier.name : "";
				if (TypePtr ctor = env.lookup_ctor(name)) {
					result = freshen_type(ctor);
					break;
				}
				if (const ValInfo* val = env.lookup_value(name)) {
					result = val->type;
					break;
				}
				report_semantic_error(node, std::string("undefined identifier: ") + name);
				result = unify.fresh();
				break;
			}
			case AST_ASSIGN:
			case AST_CONST:
				if (node->data.assign.left && node->data.assign.left->type == AST_IDENTIFIER &&
					(node->data.assign.left->data.identifier.name &&
					 (is_builtin_ctor(node->data.assign.left->data.identifier.name) ||
					  is_registered_ctor(node->data.assign.left->data.identifier.name)))) {
					check_adt_def(node);
					result = builtin_void;
					break;
				}
				result = check_assign(node);
				break;
			case AST_BINOP:
				result = check_binop(node);
				break;
			case AST_UNARYOP:
				result = check_unaryop(node);
				break;
			case AST_IF:
				result = check_if(node);
				break;
			case AST_WHILE:
				result = check_while(node);
				break;
			case AST_FOR:
				result = check_for(node);
				break;
			case AST_RETURN:
				result = check_return(node);
				break;
			case AST_CALL:
				result = check_call(node);
				break;
			case AST_STRUCT_LITERAL:
				result = check_struct_literal(node);
				break;
			case AST_INDEX:
				result = check_index(node);
				break;
			case AST_MEMBER_ACCESS:
				result = check_member(node);
				break;
			case AST_FUNCTION:
				result = check_function(node);
				break;
			case AST_GLOBAL:
				result = check_global(node);
				break;
			case AST_EXPRESSION_LIST:
				result = check_expression_list(node);
				break;
			default:
				result = builtin_void;
				break;
		}

		return record_node_type(node, result);
	}

	TypePtr check_expression_list(ASTNode* node) {
		if (!node || node->type != AST_EXPRESSION_LIST) {
			return builtin_void;
		}
		int count = node->data.expression_list.expression_count;
		if (count == 0) {
			return node_types[node] = Type::make_array(unify.fresh());
		}

		std::vector<TypePtr> elem_types;
		elem_types.reserve(count);
		for (int i = 0; i < count; i++) {
			elem_types.push_back(check_expr(node->data.expression_list.expressions[i]));
		}

		if (count == 1) {
			return node_types[node] = Type::make_array(elem_types[0]);
		}

		bool all_compatible = true;
		for (int i = 1; i < count; i++) {
			TypePtr a = unify.apply(elem_types[0]);
			TypePtr b = unify.apply(elem_types[i]);
			if (a->kind != b->kind) {
				all_compatible = false;
				break;
			}
			if (a->kind == TypeKind::Struct && b->kind == TypeKind::Struct &&
				a->data.struct_data.name != b->data.struct_data.name) {
				all_compatible = false;
				break;
			}
		}

		if (all_compatible) {
			TypePtr common = elem_types[0];
			for (int i = 1; i < count; i++) {
				try {
					unify.unify(common, elem_types[i]);
				} catch (...) {
					all_compatible = false;
					break;
				}
			}
			if (all_compatible) {
				return node_types[node] = Type::make_array(unify.apply(common));
			}
		}

		return node_types[node] = Type::make_tuple(std::move(elem_types));
	}

	TypePtr check_assign(ASTNode* node) {
		if (!node) {
			return builtin_void;
		}
		if (node->data.assign.is_declaration == 2) {
			return node_types[node] = builtin_void;
		}
		ASTNode* left = node->data.assign.left;
		ASTNode* right = node->data.assign.right;
		if (!left) {
			report_semantic_error(node, "invalid assignment: missing left-hand side");
			return node_types[node] = builtin_void;
		}
		TypePtr rtype = right ? check_expr(right) : builtin_void;

		if (node->data.assign.declared_type) {
			TypePtr annotated = type_from_ast(node->data.assign.declared_type);
			if (annotated && right) {
				try {
					unify.unify(annotated, rtype);
				} catch (const std::exception& ex) {
					TypePtr resolved_ann = unify.apply(annotated);
					TypePtr resolved_r = unify.apply(rtype);
					bool ann_is_num = is_numeric(resolved_ann);
					bool r_is_num = is_numeric(resolved_r);
					if (ann_is_num && r_is_num) {
						// ok
					}
					else if ((resolved_ann->kind == TypeKind::FixedArray && resolved_r->kind == TypeKind::Array) ||
							 (resolved_ann->kind == TypeKind::Array && resolved_r->kind == TypeKind::FixedArray)) {
						// ok
					}
					else if ((resolved_ann->kind == TypeKind::String && resolved_r->kind == TypeKind::Ptr) ||
							 (resolved_ann->kind == TypeKind::Ptr && resolved_r->kind == TypeKind::String)) {
						// ok
					}
					else if (resolved_ann->kind == TypeKind::FixedArray && resolved_r->kind == TypeKind::Ptr) {
						// nil can coerce to fixed array (all zeros)
					}
					else {
						report_type_error(node,
							std::string("type mismatch in let binding: ") + ex.what());
					}
				}
			}
			rtype = unify.apply(annotated);
		}

		if (left && left->type == AST_IDENTIFIER && right && right->type == AST_IDENTIFIER) {
			const char* rhs_name = right->data.identifier.name;
			if (rhs_name) {
				auto it = match_payloads.find(rhs_name);
				if (it != match_payloads.end()) {
					rtype = it->second;
				}
			}
		}

		if (left && left->type == AST_IDENTIFIER && right && right->type == AST_MEMBER_ACCESS) {
			ASTNode* obj = right->data.member_access.object;
			ASTNode* fld = right->data.member_access.field;
			if (obj && obj->type == AST_IDENTIFIER && obj->data.identifier.name &&
				fld && fld->type == AST_IDENTIFIER && fld->data.identifier.name) {
				std::string key = std::string(obj->data.identifier.name) + "." + std::string(fld->data.identifier.name);
				auto it = match_payload_field_types.find(key);
				if (it != match_payload_field_types.end()) {
					rtype = it->second;
				}
			}
		}

		if (left && left->type == AST_IDENTIFIER) {
			const char* name = left->data.identifier.name ? left->data.identifier.name : "";
			bool is_decl = node->data.assign.is_declaration != 0;
			if (is_decl) {
				if (!env.declare_value(name, rtype, left->mutability == MUTABILITY_MUTABLE, false)) {
					report_semantic_error(node, std::string("redefinition: ") + name);
				}
			} else {
				ValInfo* val = env.lookup_value(name);
				if (!val) {
					report_semantic_error(node, std::string("assignment to undeclared variable: ") + name);
				} else if (!val->is_mutable) {
					report_semantic_error(node, std::string("cannot assign to immutable variable '") + name + "'\n  Fix: declare with 'let mut' to make it mutable");
				} else {
					try {
						unify.unify(val->type, rtype);
					} catch (const std::exception& ex) {
						TypePtr lresolved = unify.apply(val->type);
						TypePtr rresolved = unify.apply(rtype);
						if (!((lresolved->kind == TypeKind::String && rresolved->kind == TypeKind::Ptr) ||
						      (lresolved->kind == TypeKind::Ptr && rresolved->kind == TypeKind::String))) {
							report_type_error(node, ex.what());
						}
					}
				}
			}
		} else if (left) {
			TypePtr ltype = check_expr(left);
			if (left->type == AST_UNARYOP && left->data.unaryop.op == OP_DEREF) {
				ASTNode* ptr_expr = left->data.unaryop.expr;
				if (ptr_expr && ptr_expr->type == AST_IDENTIFIER && ptr_expr->data.identifier.name) {
					const char* ptr_name = ptr_expr->data.identifier.name;
					ValInfo* ptr_val = env.lookup_value(ptr_name);
					if (ptr_val && !ptr_val->is_mutable) {
						report_semantic_error(node,
							std::string("cannot assign through pointer '") + ptr_name +
							"' because the pointer variable is immutable\n"
							"  Fix: declare with 'let mut' to make the pointer mutable");
					}
				}
			}
			try {
				unify.unify(ltype, rtype);
			} catch (const std::exception& ex) {
				TypePtr lresolved = unify.apply(ltype);
				TypePtr rresolved = unify.apply(rtype);
				if (!((lresolved->kind == TypeKind::String && rresolved->kind == TypeKind::Ptr) ||
				      (lresolved->kind == TypeKind::Ptr && rresolved->kind == TypeKind::String))) {
					report_type_error(node, ex.what());
				}
			}
		}

		return node_types[node] = builtin_void;
	}

	TypePtr check_binop(ASTNode* node) {
		TypePtr lhs = check_expr(node->data.binop.left);
		TypePtr rhs = check_expr(node->data.binop.right);
		BinOpType op = node->data.binop.op;

		if (op == OP_AND || op == OP_OR) {
			ensure_bool(node->data.binop.left, lhs);
			ensure_bool(node->data.binop.right, rhs);
			return node_types[node] = builtin_bool;
		}

		if (op == OP_EQ || op == OP_NE || op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE) {
			bool skip_unify = false;
			const char* ctor_name = nullptr;
			ASTNode* left_node = node->data.binop.left;
			ASTNode* right_node = node->data.binop.right;
			if (left_node && left_node->type == AST_IDENTIFIER &&
				(is_builtin_ctor(left_node->data.identifier.name) ||
				 vix_adt_ctor_index(left_node->data.identifier.name) >= 0)) {
				ctor_name = left_node->data.identifier.name;
				skip_unify = true;
			} else if (right_node && right_node->type == AST_IDENTIFIER &&
				(is_builtin_ctor(right_node->data.identifier.name) ||
				 vix_adt_ctor_index(right_node->data.identifier.name) >= 0)) {
				ctor_name = right_node->data.identifier.name;
				skip_unify = true;
			}
			if (!skip_unify) {
				bool none_cmp = false;
				ASTNode* lit = nullptr;
				ASTNode* other = nullptr;
				if (left_node && left_node->type == AST_NUM_INT) {
					lit = left_node;
					other = right_node;
				} else if (right_node && right_node->type == AST_NUM_INT) {
					lit = right_node;
					other = left_node;
				} else if (left_node && left_node->type == AST_NIL) {
					other = right_node;
				} else if (right_node && right_node->type == AST_NIL) {
					other = left_node;
				}
				if (other) {
					bool is_nil_or_zero = (lit && lit->data.num_int.value == 0) ||
										  (left_node && left_node->type == AST_NIL) ||
										  (right_node && right_node->type == AST_NIL);
					if (is_nil_or_zero) {
						TypePtr other_t = check_expr(other);
						TypePtr resolved = unify.apply(other_t);
						if (resolved->kind == TypeKind::App && resolved->data.app.ctor &&
							resolved->data.app.ctor->kind == TypeKind::Struct) {
							const std::string& base = resolved->data.app.ctor->data.struct_data.name;
							if (base == "Option" || base == "Result") {
								none_cmp = true;
							}
						}
						if (resolved->kind == TypeKind::Ptr || resolved->kind == TypeKind::String ||
							resolved->kind == TypeKind::Var) {
							none_cmp = true;
						}
					}
				}
			if (none_cmp) {
				return node_types[node] = builtin_bool;
			}
			if (is_numeric(lhs) && is_numeric(rhs)) {
				// Comparison operators allow numeric promotion (i32 vs i64, etc.)
			} else {
				try {
					unify.unify(lhs, rhs);
				} catch (const std::exception& ex) {
					report_type_error(node, ex.what());
				}
			}
		}
		(void)ctor_name;
		return node_types[node] = builtin_bool;
		}

		if (op == OP_CONCAT) {
			try {
				unify.unify(lhs, builtin_string);
				unify.unify(rhs, builtin_string);
			} catch (const std::exception& ex) {
				report_type_error(node, ex.what());
			}
			return node_types[node] = builtin_string;
		}

		if (op == OP_ADD) {
			TypePtr rl = unify.apply(lhs);
			TypePtr rr = unify.apply(rhs);
			bool has_string_operand = (rl->kind == TypeKind::String || rr->kind == TypeKind::String);
			if (has_string_operand && is_string_compatible(lhs) && is_string_compatible(rhs)) {
				try {
					if (rl->kind == TypeKind::Var) unify.unify(lhs, builtin_string);
					if (rr->kind == TypeKind::Var) unify.unify(rhs, builtin_string);
				} catch (...) {}
				return node_types[node] = builtin_string;
			}
		}

		if (is_numeric(lhs) && is_numeric(rhs)) {
			TypePtr res = promote_numeric(lhs, rhs);
			return node_types[node] = res;
		}

		{
			TypePtr rl = unify.apply(lhs);
			TypePtr rr = unify.apply(rhs);
			if (rl->kind == TypeKind::Ptr && is_numeric(rhs)) {
				return node_types[node] = rl;
			}
			if (rr->kind == TypeKind::Ptr && is_numeric(lhs)) {
				return node_types[node] = rr;
			}
			if (rl->kind == TypeKind::Var || rr->kind == TypeKind::Var) {
				try {
					unify.unify(lhs, rhs);
				} catch (const std::exception& ex) {
					report_type_error(node, ex.what());
				}
				return node_types[node] = unify.apply(lhs);
			}
		}

		report_type_error(node, "binary operator applied to non-numeric operands");
		return node_types[node] = unify.fresh();
	}

	TypePtr check_unaryop(ASTNode* node) {
		if (!node || !node->data.unaryop.expr) {
			return builtin_void;
		}
		TypePtr inner = check_expr(node->data.unaryop.expr);
		switch (node->data.unaryop.op) {
			case OP_ADDRESS:
				return node_types[node] = Type::make_ptr(inner);
			case OP_DEREF: {
				TypePtr resolved = unify.apply(inner);
				if (resolved->kind == TypeKind::Ptr) {
					return node_types[node] = resolved->data.ptr.pointee;
				}
				if (resolved->kind == TypeKind::Var) {
					TypePtr pointee = unify.fresh();
					TypePtr ptr = Type::make_ptr(pointee);
					try {
						unify.unify(inner, ptr);
					} catch (const std::exception& ex) {
						report_type_error(node, ex.what());
					}
					return node_types[node] = pointee;
				}
				if (resolved->kind == TypeKind::I32 || resolved->kind == TypeKind::I64 ||
					resolved->kind == TypeKind::I8 || resolved->kind == TypeKind::F32 ||
					resolved->kind == TypeKind::F64 || resolved->kind == TypeKind::Bool ||
					resolved->kind == TypeKind::String) {
					report_type_error(node,
						"cannot dereference non-pointer type (use '@' only on pointer types)");
				} else {
					report_type_error(node, "cannot dereference non-pointer");
				}
				return node_types[node] = unify.fresh();
			}
			case OP_MINUS:
			case OP_PLUS:
				if (!is_numeric(inner)) {
					TypePtr resolved_inner = unify.apply(inner);
					if (resolved_inner->kind != TypeKind::Var) {
						report_type_error(node, "unary operator expects numeric operand");
					}
				}
				return node_types[node] = inner;
			case OP_NOT:
				ensure_bool(node, inner);
				return node_types[node] = builtin_bool;
		}
		return node_types[node] = inner;
	}

	TypePtr check_if(ASTNode* node) {
		TypePtr cond = check_expr(node->data.if_stmt.condition);
		ensure_bool(node->data.if_stmt.condition, cond);

		bool pushed_match_binding = false;
		std::string scrutinee_name;
		TypePtr payload_type;

		if (node->data.if_stmt.condition && node->data.if_stmt.condition->type == AST_BINOP &&
			(node->data.if_stmt.condition->data.binop.op == OP_EQ ||
			 node->data.if_stmt.condition->data.binop.op == OP_NE)) {
			int is_ne = (node->data.if_stmt.condition->data.binop.op == OP_NE);
			ASTNode* lhs = node->data.if_stmt.condition->data.binop.left;
			ASTNode* rhs = node->data.if_stmt.condition->data.binop.right;
			ASTNode* ctor_node = nullptr;
			ASTNode* scrutinee_node = nullptr;
			bool is_nil_cmp = false;
			if (lhs && lhs->type == AST_IDENTIFIER && lhs->data.identifier.name &&
				(is_builtin_ctor(lhs->data.identifier.name) || vix_adt_ctor_index(lhs->data.identifier.name) >= 0)) {
				ctor_node = lhs;
				scrutinee_node = rhs;
			} else if (rhs && rhs->type == AST_IDENTIFIER && rhs->data.identifier.name &&
				(is_builtin_ctor(rhs->data.identifier.name) || vix_adt_ctor_index(rhs->data.identifier.name) >= 0)) {
				ctor_node = rhs;
				scrutinee_node = lhs;
			} else if (lhs && lhs->type == AST_NIL && rhs && rhs->type == AST_IDENTIFIER) {
				is_nil_cmp = true;
				scrutinee_node = rhs;
			} else if (rhs && rhs->type == AST_NIL && lhs && lhs->type == AST_IDENTIFIER) {
				is_nil_cmp = true;
				scrutinee_node = lhs;
			}
			if ((ctor_node || is_nil_cmp) && scrutinee_node && scrutinee_node->type == AST_IDENTIFIER) {
				scrutinee_name = scrutinee_node->data.identifier.name ? scrutinee_node->data.identifier.name : "";
				if (!scrutinee_name.empty()) {
					TypePtr scrutinee_type = check_expr(scrutinee_node);
					TypePtr resolved = unify.apply(scrutinee_type);
					if (is_nil_cmp) {
						if (resolved->kind == TypeKind::App && resolved->data.app.ctor &&
							resolved->data.app.ctor->kind == TypeKind::Struct) {
							const std::string& base = resolved->data.app.ctor->data.struct_data.name;
							if (base == "Option" && resolved->data.app.args.size() == 1 && is_ne) {
								payload_type = resolved->data.app.args[0];
							} else if (base == "Result" && resolved->data.app.args.size() == 2 && is_ne) {
								payload_type = resolved->data.app.args[0];
							}
						}
						if (resolved->kind == TypeKind::Ptr && is_ne) {
							payload_type = resolved;
						}
					} else {
						const char* ctor_name = ctor_node->data.identifier.name;
						if (resolved->kind == TypeKind::App && resolved->data.app.ctor &&
							resolved->data.app.ctor->kind == TypeKind::Struct) {
							const std::string& base = resolved->data.app.ctor->data.struct_data.name;
							if (base == "Option" && resolved->data.app.args.size() == 1) {
								if (is_ne && strcmp(ctor_name, "None") == 0) {
									payload_type = resolved->data.app.args[0];
								} else if (!is_ne && strcmp(ctor_name, "Some") == 0) {
									payload_type = resolved->data.app.args[0];
								} else if (!is_ne && strcmp(ctor_name, "None") != 0) {
									payload_type = resolved->data.app.args[0];
								}
						} else if (base == "Result" && resolved->data.app.args.size() == 2) {
							if (is_ne) {
								payload_type = resolved->data.app.args[0];
							} else if (strcmp(ctor_name, "Ok") == 0) {
								payload_type = resolved->data.app.args[0];
							} else if (strcmp(ctor_name, "Err") == 0) {
								payload_type = resolved->data.app.args[1];
							}
						} else {
							/* Custom ADT: look up payload type from ADT definition */
							ASTNode* payload_node = vix_adt_ctor_payload_type_node(ctor_name);
							if (payload_node) {
								payload_type = type_from_ast(payload_node);
							}
						}
						}
					}
					if (payload_type) {
						match_payloads[scrutinee_name] = payload_type;
						match_payload_field_types[std::string(scrutinee_name) + ".1"] = payload_type;
						match_payload_field_types[std::string(scrutinee_name) + ".0"] = builtin_i32;
						pushed_match_binding = true;
					}
				}
			}
		}

		// Detect desugared match pattern: x.0 == CtorName (from match desugaring)
		if (!pushed_match_binding && node->data.if_stmt.condition &&
			node->data.if_stmt.condition->type == AST_BINOP &&
			node->data.if_stmt.condition->data.binop.op == OP_EQ) {
			ASTNode* lhs = node->data.if_stmt.condition->data.binop.left;
			ASTNode* rhs = node->data.if_stmt.condition->data.binop.right;
			// Pattern: x.0 == CtorName
			ASTNode* member_node = nullptr;
			const char* ctor_name = nullptr;
			if (lhs && lhs->type == AST_MEMBER_ACCESS && rhs && rhs->type == AST_IDENTIFIER &&
				rhs->data.identifier.name &&
				(is_builtin_ctor(rhs->data.identifier.name) || vix_adt_ctor_index(rhs->data.identifier.name) >= 0)) {
				member_node = lhs;
				ctor_name = rhs->data.identifier.name;
			} else if (rhs && rhs->type == AST_MEMBER_ACCESS && lhs && lhs->type == AST_IDENTIFIER &&
				lhs->data.identifier.name &&
				(is_builtin_ctor(lhs->data.identifier.name) || vix_adt_ctor_index(lhs->data.identifier.name) >= 0)) {
				member_node = rhs;
				ctor_name = lhs->data.identifier.name;
			}
			if (member_node && ctor_name && member_node->data.member_access.field &&
				member_node->data.member_access.field->type == AST_IDENTIFIER &&
				member_node->data.member_access.field->data.identifier.name &&
				strcmp(member_node->data.member_access.field->data.identifier.name, "0") == 0) {
				ASTNode* obj = member_node->data.member_access.object;
				if (obj && obj->type == AST_IDENTIFIER && obj->data.identifier.name) {
					scrutinee_name = obj->data.identifier.name;
					TypePtr scrutinee_type = check_expr(obj);
					TypePtr resolved = unify.apply(scrutinee_type);
					if (resolved->kind == TypeKind::App && resolved->data.app.ctor &&
						resolved->data.app.ctor->kind == TypeKind::Struct) {
						const std::string& base = resolved->data.app.ctor->data.struct_data.name;
						if (base == "Result" && resolved->data.app.args.size() == 2) {
							if (strcmp(ctor_name, "Ok") == 0) {
								payload_type = resolved->data.app.args[0];
							} else if (strcmp(ctor_name, "Err") == 0) {
								payload_type = resolved->data.app.args[1];
							}
						} else if (base == "Option" && resolved->data.app.args.size() == 1) {
							if (strcmp(ctor_name, "Some") == 0) {
								payload_type = resolved->data.app.args[0];
							}
						} else {
							/* Custom ADT: look up payload type from ADT definition */
							ASTNode* payload_node = vix_adt_ctor_payload_type_node(ctor_name);
							if (payload_node) {
								payload_type = type_from_ast(payload_node);
							}
						}
					} else if (resolved->kind == TypeKind::Struct) {
						/* Custom ADT with Struct type (not App): look up payload type */
						ASTNode* payload_node = vix_adt_ctor_payload_type_node(ctor_name);
						if (payload_node) {
							payload_type = type_from_ast(payload_node);
						}
					}
					if (payload_type) {
						match_payloads[scrutinee_name] = payload_type;
						match_payload_field_types[std::string(scrutinee_name) + ".1"] = payload_type;
						match_payload_field_types[std::string(scrutinee_name) + ".0"] = builtin_i32;
						pushed_match_binding = true;
					}
				}
			}
		}

		TypePtr then_t = check_block(node->data.if_stmt.then_body, true);
		if (pushed_match_binding) {
			match_payloads.erase(scrutinee_name);
			match_payload_field_types.erase(scrutinee_name + ".0");
			match_payload_field_types.erase(scrutinee_name + ".1");
		}
		TypePtr else_t = node->data.if_stmt.else_body ? check_block(node->data.if_stmt.else_body, true) : builtin_void;
		if (node->data.if_stmt.else_body) {
			try {
				unify.unify(then_t, else_t);
			} catch (const std::exception& ex) {
				TypePtr rt = unify.apply(then_t);
				TypePtr re = unify.apply(else_t);
				if (rt->kind == TypeKind::Void && re->kind != TypeKind::Void) {
					return node_types[node] = re;
				}
				if (re->kind == TypeKind::Void && rt->kind != TypeKind::Void) {
					return node_types[node] = rt;
				}
				report_type_error(node, ex.what());
			}
			return node_types[node] = unify.apply(then_t);
		}
		return node_types[node] = builtin_void;
	}

	TypePtr check_while(ASTNode* node) {
		TypePtr cond = check_expr(node->data.while_stmt.condition);
		ensure_bool(node->data.while_stmt.condition, cond);
		check_expr(node->data.while_stmt.body);
		return node_types[node] = builtin_void;
	}

	TypePtr check_for(ASTNode* node) {
		if (!node->data.for_stmt.var || node->data.for_stmt.var->type != AST_IDENTIFIER) {
			report_semantic_error(node, "invalid for-loop variable");
			return node_types[node] = builtin_void;
		}
		env.enter_scope();
		TypePtr iter_type = check_expr(node->data.for_stmt.start);
		TypePtr var_type = unify.fresh();
		if (node->data.for_stmt.end) {
			TypePtr end_type = check_expr(node->data.for_stmt.end);
			TypePtr resolved_iter = unify.apply(iter_type);
			TypePtr resolved_end = unify.apply(end_type);
			if (is_numeric(resolved_iter) && is_numeric(resolved_end)) {
				TypePtr promoted = promote_numeric(resolved_iter, resolved_end);
				try {
					unify.unify(iter_type, promoted);
					unify.unify(end_type, promoted);
				} catch (const std::exception& ex) {
					report_type_error(node, ex.what());
				}
				var_type = promoted;
			} else {
				try {
					unify.unify(iter_type, end_type);
				} catch (const std::exception& ex) {
					report_type_error(node, ex.what());
				}
				var_type = iter_type;
			}
		} else {
			if (iter_type->kind == TypeKind::Array) {
				var_type = iter_type->data.array.element;
			} else if (iter_type->kind == TypeKind::FixedArray) {
				var_type = iter_type->data.fixed_array.element;
			} else {
				var_type = unify.fresh();
			}
		}
		env.declare_value(node->data.for_stmt.var->data.identifier.name, var_type, false, false);
		check_expr(node->data.for_stmt.body);
		env.exit_scope();
		return node_types[node] = builtin_void;
	}

	TypePtr check_return(ASTNode* node) {
		if (node->data.return_stmt.expr) {
			return node_types[node] = check_expr(node->data.return_stmt.expr);
		}
		return node_types[node] = builtin_void;
	}

	TypePtr check_index(ASTNode* node) {
		TypePtr target = check_expr(node->data.index.target);
		TypePtr idx = check_expr(node->data.index.index);
		try {
			unify.unify(idx, builtin_i32);
		} catch (const std::exception& ex) {
			report_type_error(node, ex.what());
		}
		TypePtr resolved = unify.apply(target);
		if (resolved->kind == TypeKind::Array) {
			return node_types[node] = resolved->data.array.element;
		}
		if (resolved->kind == TypeKind::FixedArray) {
			return node_types[node] = resolved->data.fixed_array.element;
		}
		if (resolved->kind == TypeKind::Ptr) {
			TypePtr pointee = unify.apply(resolved->data.ptr.pointee);
			if (pointee->kind == TypeKind::Array) {
				return node_types[node] = pointee->data.array.element;
			}
			if (pointee->kind == TypeKind::FixedArray) {
				return node_types[node] = pointee->data.fixed_array.element;
			}
			return node_types[node] = pointee;
		}
		if (resolved->kind == TypeKind::String) {
			return node_types[node] = builtin_i8;
		}
		report_type_error(node, "indexing non-array value");
		return node_types[node] = unify.fresh();
	}

	TypePtr check_member(ASTNode* node) {
		TypePtr target = check_expr(node->data.member_access.object);
		TypePtr resolved = unify.apply(target);
		const char* field = node->data.member_access.field &&
							node->data.member_access.field->type == AST_IDENTIFIER
								? node->data.member_access.field->data.identifier.name
								: "";

		auto set_member_type = [&](const TypePtr& t) -> TypePtr {
			TypePtr r = t ? unify.apply(t) : unify.fresh();
			node_types[node] = r;
			store_inferred_type(node, r);
			return r;
		};

		bool numeric_field = field && field[0] != '\0';
		for (const char* p = field; numeric_field && *p; ++p) {
			if (*p < '0' || *p > '9') {
				numeric_field = false;
			}
		}

		if (numeric_field) {
			if (resolved->kind == TypeKind::Array) {
				return set_member_type(resolved->data.array.element);
			}
			if (resolved->kind == TypeKind::FixedArray) {
				return set_member_type(resolved->data.fixed_array.element);
			}
			if (resolved->kind == TypeKind::Ptr) {
				TypePtr pointee = unify.apply(resolved->data.ptr.pointee);
				if (pointee->kind == TypeKind::Array) {
					return set_member_type(pointee->data.array.element);
				}
				if (pointee->kind == TypeKind::FixedArray) {
					return set_member_type(pointee->data.fixed_array.element);
				}
				return set_member_type(pointee);
			}
			if (resolved->kind == TypeKind::Tuple) {
				int idx = atoi(field);
				if (idx >= 0 && idx < (int)resolved->data.tuple.elements.size()) {
					return set_member_type(resolved->data.tuple.elements[idx]);
				}
				report_semantic_error(node, "tuple index out of bounds");
				return set_member_type(unify.fresh());
			}
			if (resolved->kind == TypeKind::App && resolved->data.app.ctor &&
				resolved->data.app.ctor->kind == TypeKind::Struct) {
				const std::string& base = resolved->data.app.ctor->data.struct_data.name;
				int idx = atoi(field);
				if (idx == 0) {
					return set_member_type(builtin_i32);
				}
				if (idx == 1 && !resolved->data.app.args.empty()) {
					// Check match_payload_field_types first (set by check_if for desugared match)
					ASTNode* objectNode = node->data.member_access.object;
					if (objectNode && objectNode->type == AST_IDENTIFIER && objectNode->data.identifier.name) {
						std::string key = std::string(objectNode->data.identifier.name) + "." + std::string(field);
						auto it = match_payload_field_types.find(key);
						if (it != match_payload_field_types.end()) {
							return set_member_type(it->second);
						}
					}
					if (base == "Option" && resolved->data.app.args.size() >= 1) {
						return set_member_type(resolved->data.app.args[0]);
					}
					if (base == "Result" && resolved->data.app.args.size() >= 2) {
						return set_member_type(resolved->data.app.args[0]);
					}
					/* Custom ADT: look up payload type from ADT definition */
					if (objectNode && objectNode->type == AST_IDENTIFIER && objectNode->data.identifier.name) {
						/* Try to find the constructor from the match context */
						ASTNode* payload_node = vix_adt_ctor_payload_type_node(base.c_str());
						if (!payload_node) {
							payload_node = vix_adt_payload_type_for_base(base.c_str());
						}
						if (payload_node) {
							return set_member_type(type_from_ast(payload_node));
						}
					}
				}
				{
					ASTNode* objectNode = node->data.member_access.object;
					if (objectNode && objectNode->type == AST_IDENTIFIER && objectNode->data.identifier.name) {
						std::string key = std::string(objectNode->data.identifier.name) + "." + std::string(field);
						auto it = match_payload_field_types.find(key);
						if (it != match_payload_field_types.end()) {
							return set_member_type(it->second);
						}
					}
				}
				return set_member_type(unify.fresh());
			}
			{
				ASTNode* objectNode = node->data.member_access.object;
				if (objectNode && objectNode->type == AST_IDENTIFIER && objectNode->data.identifier.name) {
					std::string key = std::string(objectNode->data.identifier.name) + "." + std::string(field);
					auto it = match_payload_field_types.find(key);
					if (it != match_payload_field_types.end()) {
						return set_member_type(it->second);
					}
				}
			}
		}

		if (strcmp(field, "length") == 0 || strcmp(field, "size") == 0) {
			return node_types[node] = builtin_i32;
		}

		if (strcmp(field, "push") == 0) {
			TypePtr elem = unify.fresh();
			if (resolved->kind == TypeKind::Array) {
				elem = resolved->data.array.element;
			} else if (resolved->kind == TypeKind::FixedArray) {
				elem = resolved->data.fixed_array.element;
			} else if (resolved->kind == TypeKind::Ptr) {
				TypePtr pointee = unify.apply(resolved->data.ptr.pointee);
				if (pointee->kind == TypeKind::Array) {
					elem = pointee->data.array.element;
				} else if (pointee->kind == TypeKind::FixedArray) {
					elem = pointee->data.fixed_array.element;
				}
			}
			return node_types[node] = Type::make_fn({elem}, builtin_void);
		}

		if (resolved->kind == TypeKind::Ptr) {
			TypePtr pointee = unify.apply(resolved->data.ptr.pointee);
			if (pointee->kind == TypeKind::Struct) {
				resolved = pointee;
			} else if (pointee->kind == TypeKind::App && pointee->data.app.ctor &&
					   pointee->data.app.ctor->kind == TypeKind::Struct) {
				resolved = pointee;
			}
		}

		std::vector<TypePtr> app_args;
		if (resolved->kind == TypeKind::App && resolved->data.app.ctor &&
			resolved->data.app.ctor->kind == TypeKind::Struct) {
			app_args = resolved->data.app.args;
			resolved = resolved->data.app.ctor;
		}

		if (resolved->kind != TypeKind::Struct) {
			report_type_error(node, "member access on non-struct");
			return node_types[node] = unify.fresh();
		}
		const StructInfo* info = env.lookup_struct(resolved->data.struct_data.name);
		if (!info) {
			if (vix_is_adt_definition(resolved->data.struct_data.name.c_str())) {
				return node_types[node] = unify.fresh();
			}
			report_semantic_error(node, "unknown struct: " + resolved->data.struct_data.name);
			return node_types[node] = unify.fresh();
		}
		for (const auto& f : info->fields) {
			if (f.name == field) {
				if (!app_args.empty() && !info->generic_param_ids.empty()) {
					std::unordered_map<int, TypePtr> mapping;
					for (size_t gi = 0; gi < info->generic_param_ids.size() && gi < app_args.size(); ++gi) {
						mapping[info->generic_param_ids[gi]] = app_args[gi];
					}
					auto rewrite = [&](const TypePtr& t, auto& self) -> TypePtr {
						if (!t) return t;
						TypePtr a = unify.apply(t);
						if (a->kind == TypeKind::Var) {
							auto it = mapping.find(a->data.var.id);
							if (it != mapping.end()) return it->second;
							return a;
						}
						switch (a->kind) {
							case TypeKind::Ptr:
								return Type::make_ptr(self(a->data.ptr.pointee, self));
							case TypeKind::Array:
								return Type::make_array(self(a->data.array.element, self));
							case TypeKind::FixedArray:
								return Type::make_fixed_array(self(a->data.fixed_array.element, self),
															  a->data.fixed_array.size);
							case TypeKind::App: {
								std::vector<TypePtr> args;
								args.reserve(a->data.app.args.size());
								for (const auto& arg : a->data.app.args) {
									args.push_back(self(arg, self));
								}
								return Type::make_app(self(a->data.app.ctor, self), std::move(args));
							}
							case TypeKind::Fn: {
								std::vector<TypePtr> params;
								params.reserve(a->data.fn.params.size());
								for (const auto& param : a->data.fn.params) {
									params.push_back(self(param, self));
								}
								return Type::make_fn(std::move(params), self(a->data.fn.ret, self),
													 a->data.fn.generic_param_ids, a->data.fn.vararg);
							}
							case TypeKind::Tuple: {
								std::vector<TypePtr> elems;
								elems.reserve(a->data.tuple.elements.size());
								for (const auto& elem : a->data.tuple.elements) {
									elems.push_back(self(elem, self));
								}
								return Type::make_tuple(std::move(elems));
							}
							default:
								return a;
						}
					};
					TypePtr ft = rewrite(f.type, rewrite);
					return set_member_type(ft);
				}
				return set_member_type(f.type);
			}
		}
		report_semantic_error(node, std::string("unknown field: ") + field);
		return node_types[node] = unify.fresh();
	}

	TypePtr check_struct_literal(ASTNode* node) {
		if (!node->data.struct_literal.type_name) {
			report_semantic_error(node, "struct literal without name");
			return node_types[node] = unify.fresh();
		}
		const char* name = nullptr;
		if (node->data.struct_literal.type_name->type == AST_IDENTIFIER) {
			name = node->data.struct_literal.type_name->data.identifier.name;
		} else if (node->data.struct_literal.type_name->type == AST_TYPE_APP &&
				node->data.struct_literal.type_name->data.type_app.ctor &&
				node->data.struct_literal.type_name->data.type_app.ctor->type == AST_IDENTIFIER) {
			name = node->data.struct_literal.type_name->data.type_app.ctor->data.identifier.name;
		}
		if (!name) {
			report_semantic_error(node, "struct literal without name");
			return node_types[node] = unify.fresh();
		}
		const StructInfo* info = env.lookup_struct(name);
		if (!info) {
			if (vix_is_adt_definition(name)) {
				return node_types[node] = unify.fresh();
			}
			report_semantic_error(node, std::string("unknown struct: ") + name);
			return node_types[node] = unify.fresh();
		}

		std::map<int, TypePtr> generic_subst;
		if (node->data.struct_literal.type_name->type == AST_TYPE_APP && !info->generic_param_ids.empty()) {
			ASTNode* arg_list = node->data.struct_literal.type_name->data.type_app.args;
			if (arg_list && arg_list->type == AST_EXPRESSION_LIST) {
				int arg_count = arg_list->data.expression_list.expression_count;
				int param_count = static_cast<int>(info->generic_param_ids.size());
				int n = std::min(arg_count, param_count);
				for (int i = 0; i < n; i++) {
					TypePtr concrete = type_from_ast(arg_list->data.expression_list.expressions[i]);
					generic_subst[info->generic_param_ids[i]] = concrete;
				}
			}
		}

		auto subst_type = [&](const TypePtr& t, auto& self_ref) -> TypePtr {
			if (!t) return t;
			TypePtr a = unify.apply(t);
			if (a->kind == TypeKind::Var) {
				auto it = generic_subst.find(a->data.var.id);
				if (it != generic_subst.end()) return it->second;
				return a;
			}
			if (a->kind == TypeKind::Array) {
				return Type::make_array(self_ref(a->data.array.element, self_ref));
			}
			if (a->kind == TypeKind::Ptr) {
				return Type::make_ptr(self_ref(a->data.ptr.pointee, self_ref));
			}
			if (a->kind == TypeKind::App) {
				std::vector<TypePtr> new_args;
				new_args.reserve(a->data.app.args.size());
				for (const auto& arg : a->data.app.args) {
					new_args.push_back(self_ref(arg, self_ref));
				}
				return Type::make_app(self_ref(a->data.app.ctor, self_ref), std::move(new_args));
			}
			if (a->kind == TypeKind::Fn) {
				std::vector<TypePtr> new_params;
				new_params.reserve(a->data.fn.params.size());
				for (const auto& p : a->data.fn.params) {
					new_params.push_back(self_ref(p, self_ref));
				}
				return Type::make_fn(std::move(new_params), self_ref(a->data.fn.ret, self_ref));
			}
			return a;
		};

		if (node->data.struct_literal.fields && node->data.struct_literal.fields->type == AST_EXPRESSION_LIST) {
			int count = node->data.struct_literal.fields->data.expression_list.expression_count;
			for (int i = 0; i < count; i++) {
				ASTNode* field = node->data.struct_literal.fields->data.expression_list.expressions[i];
				if (!field || field->type != AST_ASSIGN) {
					continue;
				}
				ASTNode* field_name = field->data.assign.left;
				ASTNode* field_expr = field->data.assign.right;
				if (!field_name || field_name->type != AST_IDENTIFIER) {
					continue;
				}
				const char* fname = field_name->data.identifier.name;
				bool found = false;
				for (const auto& f : info->fields) {
					if (f.name == fname) {
						found = true;
						TypePtr ft = check_expr(field_expr);
						TypePtr field_type = generic_subst.empty() ? f.type : subst_type(f.type, subst_type);

						std::function<void(const TypePtr&, const TypePtr&)> coerce_tuple_to_array;
						coerce_tuple_to_array = [&](const TypePtr& expected, const TypePtr& actual) {
							TypePtr re = unify.apply(expected);
							TypePtr ra = unify.apply(actual);
							if (re && re->kind == TypeKind::Array && ra && ra->kind == TypeKind::Tuple) {
								TypePtr elem = re->data.array.element;
								for (const auto& te : ra->data.tuple.elements) {
									coerce_tuple_to_array(elem, te);
								}
							} else if (re && re->kind == TypeKind::FixedArray && ra && ra->kind == TypeKind::Ptr) {
								
							} else {
								unify.unify(expected, actual);
							}
						};

						try {
							coerce_tuple_to_array(field_type, ft);
						} catch (const std::exception& ex) {
							report_type_error(field, ex.what());
						}
						break;
					}
				}
				if (!found) {
					report_semantic_error(field, std::string("unknown field: ") + fname);
				}
			}
		}
		if (node->data.struct_literal.type_name->type == AST_TYPE_APP) {
			std::vector<TypePtr> args;
			ASTNode* arg_list = node->data.struct_literal.type_name->data.type_app.args;
			if (arg_list && arg_list->type == AST_EXPRESSION_LIST) {
				int count = arg_list->data.expression_list.expression_count;
				args.reserve(count);
				for (int i = 0; i < count; i++) {
					args.push_back(type_from_ast(arg_list->data.expression_list.expressions[i]));
				}
			}
			return node_types[node] = Type::make_app(Type::make_struct(name), std::move(args));
		}
		return node_types[node] = Type::make_struct(name);
	}

	TypePtr instantiate_fn(const TypePtr& fn_type, const std::vector<TypePtr>& type_args) {
		TypePtr resolved = fn_type;
		if (resolved->kind != TypeKind::Fn) {
			resolved = unify.apply(fn_type);
		}
		if (resolved->kind != TypeKind::Fn) {
			return resolved;
		}
		if (resolved->data.fn.generic_param_ids.empty()) {
			return resolved;
		}
		std::unordered_set<int> generic_id_set(resolved->data.fn.generic_param_ids.begin(),
											   resolved->data.fn.generic_param_ids.end());
		std::unordered_map<int, TypePtr> mapping;
		for (size_t i = 0; i < resolved->data.fn.generic_param_ids.size() && i < type_args.size(); ++i) {
			mapping[resolved->data.fn.generic_param_ids[i]] = type_args[i];
		}
		auto rewrite = [&](const TypePtr& t, auto& rewrite_ref) -> TypePtr {
			if (!t) {
				return t;
			}
			if (t->kind == TypeKind::Var) {
				auto it = mapping.find(t->data.var.id);
				if (it != mapping.end()) {
					return it->second;
				}
			}
			TypePtr a = t;
			if (generic_id_set.find(t->kind == TypeKind::Var ? t->data.var.id : -1) == generic_id_set.end()) {
				a = unify.apply(t);
			}
			if (a->kind == TypeKind::Var) {
				auto it = mapping.find(a->data.var.id);
				if (it != mapping.end()) {
					return it->second;
				}
				return a;
			}
			switch (a->kind) {
				case TypeKind::Ptr:
					return Type::make_ptr(rewrite_ref(a->data.ptr.pointee, rewrite_ref));
				case TypeKind::Array:
					return Type::make_array(rewrite_ref(a->data.array.element, rewrite_ref));
				case TypeKind::FixedArray:
					return Type::make_fixed_array(rewrite_ref(a->data.fixed_array.element, rewrite_ref),
												  a->data.fixed_array.size);
				case TypeKind::App: {
					std::vector<TypePtr> args;
					args.reserve(a->data.app.args.size());
					for (const auto& arg : a->data.app.args) {
						args.push_back(rewrite_ref(arg, rewrite_ref));
					}
					return Type::make_app(rewrite_ref(a->data.app.ctor, rewrite_ref), std::move(args));
				}
				case TypeKind::Fn: {
					std::vector<TypePtr> params;
					params.reserve(a->data.fn.params.size());
					for (const auto& param : a->data.fn.params) {
						params.push_back(rewrite_ref(param, rewrite_ref));
					}
					return Type::make_fn(std::move(params), rewrite_ref(a->data.fn.ret, rewrite_ref),
										 a->data.fn.generic_param_ids, a->data.fn.vararg);
				}
				case TypeKind::Tuple: {
					std::vector<TypePtr> elems;
					elems.reserve(a->data.tuple.elements.size());
					for (const auto& elem : a->data.tuple.elements) {
						elems.push_back(rewrite_ref(elem, rewrite_ref));
					}
					return Type::make_tuple(std::move(elems));
				}
				default:
					return a;
			}
		};
		return rewrite(resolved, rewrite);
	}

	TypePtr check_call(ASTNode* node) {
		/* Handle impl method calls: obj.method(args) or Type.method(args) */
		if (node->data.call.func && node->data.call.func->type == AST_MEMBER_ACCESS) {
			ASTNode* mem = node->data.call.func;
			ASTNode* objectNode = mem->data.member_access.object;
			ASTNode* fieldNode = mem->data.member_access.field;
			if (objectNode && fieldNode && fieldNode->type == AST_IDENTIFIER && fieldNode->data.identifier.name) {
				std::string methodName(fieldNode->data.identifier.name);
				std::string typeName;
				/* Check if object is a type name (e.g., Point.new) */
				if (objectNode->type == AST_IDENTIFIER && objectNode->data.identifier.name) {
					std::string objName(objectNode->data.identifier.name);
					/* Check if it's a known type (struct or ADT) */
					if (env.lookup_struct(objName) || vix_is_adt_definition(objName.c_str())) {
						typeName = objName;
					}
				}
				/* If not a type name, try to get the type from the object */
				if (typeName.empty()) {
					TypePtr objType = check_expr(objectNode);
					TypePtr resolved = unify.apply(objType);
					if (resolved->kind == TypeKind::App && resolved->data.app.ctor &&
						resolved->data.app.ctor->kind == TypeKind::Struct) {
						typeName = resolved->data.app.ctor->data.struct_data.name;
					} else if (resolved->kind == TypeKind::Struct) {
						typeName = resolved->data.struct_data.name;
					} else if (resolved->kind == TypeKind::String) {
						typeName = "string";
					} else if (resolved->kind == TypeKind::I32) {
						typeName = "i32";
					} else if (resolved->kind == TypeKind::I64) {
						typeName = "i64";
					} else if (resolved->kind == TypeKind::F64) {
						typeName = "f64";
					} else if (resolved->kind == TypeKind::F32) {
						typeName = "f32";
					} else if (resolved->kind == TypeKind::I8) {
						typeName = "i8";
					}
				}
				if (!typeName.empty()) {
					const char* mangledFunc = vix_lookup_impl_method(typeName.c_str(), methodName.c_str());
					if (mangledFunc) {
						/* Look up the mangled function in the environment */
						TypePtr fnType = env.lookup_value(mangledFunc) ? env.lookup_value(mangledFunc)->type : TypePtr(nullptr);
						if (fnType && fnType->kind == TypeKind::Fn) {
							/* Check if function has 'self' parameter */
							bool hasSelf = false;
							/* Look at the function node to check parameter names */
							/* For now, assume methods with first param named 'self' are instance methods */
							/* We can check by looking at the function's parameter count vs args */
							int totalParams = (int)fnType->data.fn.params.size();
							int actualArgs = node->data.call.args ? node->data.call.args->data.expression_list.expression_count : 0;
							/* If the function has more params than args, it might have a self param */
							/* Actually, let's just try to match without self first, then with self */
							int expectedArgs = totalParams;
							/* Check if this is a static call (object is a type name) */
							bool isStaticCall = false;
							if (objectNode->type == AST_IDENTIFIER && objectNode->data.identifier.name) {
								std::string objName(objectNode->data.identifier.name);
								if (env.lookup_struct(objName) || vix_is_adt_definition(objName.c_str())) {
									isStaticCall = true;
								}
							}
							if (!isStaticCall && totalParams > 0) {
								/* Instance method: subtract self from param count */
								expectedArgs = totalParams - 1;
							}
							if (actualArgs != expectedArgs) {
								report_semantic_error(node, std::string("method '") + methodName + "' expects " +
									std::to_string(expectedArgs) + " arguments, but got " + std::to_string(actualArgs));
							}
							/* Unify arguments */
							int paramOffset = isStaticCall ? 0 : 1;
							for (int i = 0; i < actualArgs && i + paramOffset < totalParams; i++) {
								ASTNode* arg = node->data.call.args->data.expression_list.expressions[i];
								TypePtr argType = check_expr(arg);
								try {
									unify.unify(fnType->data.fn.params[i + paramOffset], argType);
								} catch (const std::exception& ex) {
									report_type_error(arg, ex.what());
								}
							}
							return node_types[node] = fnType->data.fn.ret;
						}
					}
				}
			}
		}

		if (node->data.call.func && node->data.call.func->type == AST_IDENTIFIER) {
			const char* cname = node->data.call.func->data.identifier.name;
			if (cname && strcmp(cname, "Some") == 0) {
				TypePtr arg_t = unify.fresh();
				if (node->data.call.args && node->data.call.args->type == AST_EXPRESSION_LIST &&
					node->data.call.args->data.expression_list.expression_count == 1) {
					arg_t = check_expr(node->data.call.args->data.expression_list.expressions[0]);
				} else {
					report_semantic_error(node, "Some(...) expects one argument");
				}
				return node_types[node] = Type::make_app(Type::make_struct("Option"), {arg_t});
			}
			if (cname && strcmp(cname, "None") == 0) {
				return node_types[node] = Type::make_app(Type::make_struct("Option"), {unify.fresh()});
			}
			if (cname && strcmp(cname, "Ok") == 0) {
				TypePtr arg_t = unify.fresh();
				if (node->data.call.args && node->data.call.args->type == AST_EXPRESSION_LIST &&
					node->data.call.args->data.expression_list.expression_count == 1) {
					arg_t = check_expr(node->data.call.args->data.expression_list.expressions[0]);
				} else {
					report_semantic_error(node, "Ok(...) expects one argument");
				}
				return node_types[node] = Type::make_app(Type::make_struct("Result"), {arg_t, unify.fresh()});
			}
			if (cname && strcmp(cname, "Err") == 0) {
				TypePtr arg_t = unify.fresh();
				if (node->data.call.args && node->data.call.args->type == AST_EXPRESSION_LIST &&
					node->data.call.args->data.expression_list.expression_count == 1) {
					arg_t = check_expr(node->data.call.args->data.expression_list.expressions[0]);
				} else {
					report_semantic_error(node, "Err(...) expects one argument");
				}
				return node_types[node] = Type::make_app(Type::make_struct("Result"), {unify.fresh(), arg_t});
			}
			TypePtr ctor_type = env.lookup_ctor(cname);
			if (ctor_type) {
				ctor_type = freshen_type(ctor_type);
				TypePtr resolved_ctor = unify.apply(ctor_type);
				if (resolved_ctor->kind == TypeKind::Fn) {
					size_t actual = node->data.call.args && node->data.call.args->type == AST_EXPRESSION_LIST
										? node->data.call.args->data.expression_list.expression_count
										: 0;
					size_t expected = resolved_ctor->data.fn.params.size();
					if (!resolved_ctor->data.fn.vararg && expected != actual) {
						report_semantic_error(node, std::string("constructor '") + cname + "' arity mismatch");
					}
					if (node->data.call.args && node->data.call.args->type == AST_EXPRESSION_LIST) {
						for (size_t i = 0; i < actual && i < expected; i++) {
							TypePtr arg_t = check_expr(node->data.call.args->data.expression_list.expressions[i]);
							try {
								unify.unify(resolved_ctor->data.fn.params[i], arg_t);
							} catch (const std::exception& ex) {
								report_type_error(node, ex.what());
							}
						}
					}
					return node_types[node] = resolved_ctor->data.fn.ret;
				}
				return node_types[node] = resolved_ctor;
			}
		}

		TypePtr callee = check_expr(node->data.call.func);
		bool has_type_args = node->data.call.type_args && node->data.call.type_args->type == AST_EXPRESSION_LIST &&
							 node->data.call.type_args->data.expression_list.expression_count > 0;
		TypePtr resolved = has_type_args ? callee : unify.apply(callee);

		std::vector<TypePtr> arg_types;
		size_t actual = node->data.call.args && node->data.call.args->type == AST_EXPRESSION_LIST
								? node->data.call.args->data.expression_list.expression_count
								: 0;
			if (node->data.call.args && node->data.call.args->type == AST_EXPRESSION_LIST) {
				arg_types.reserve(actual);
				for (size_t i = 0; i < actual; i++) {
					arg_types.push_back(check_expr(node->data.call.args->data.expression_list.expressions[i]));
				}
			}

		if (resolved->kind == TypeKind::Fn) {
			TypePtr fn_type = resolved;
			if (node->data.call.type_args && node->data.call.type_args->type == AST_EXPRESSION_LIST) {
				std::vector<TypePtr> type_args;
				int count = node->data.call.type_args->data.expression_list.expression_count;
				for (int i = 0; i < count; i++) {
					type_args.push_back(type_from_ast(node->data.call.type_args->data.expression_list.expressions[i]));
				}
				fn_type = instantiate_fn(resolved, type_args);
			}

			size_t expected = fn_type->data.fn.params.size();
			if (!fn_type->data.fn.vararg && expected != actual) {
				report_semantic_error(node, "function call arity mismatch");
			}

			for (size_t i = 0; i < actual && i < expected; i++) {
				TypePtr param_type = unify.apply(fn_type->data.fn.params[i]);
				TypePtr arg_type = unify.apply(arg_types[i]);
				if (param_type->kind == TypeKind::Ptr && arg_type->kind == TypeKind::String) {
					continue;
				}
				if (param_type->kind == TypeKind::Ptr && arg_type->kind == TypeKind::FixedArray) {
					// Allow passing array to ptr parameter (array decays to pointer)
					continue;
				}
				if (param_type->kind == TypeKind::Ptr && arg_type->kind == TypeKind::Array) {
					// Allow passing array to ptr parameter (array decays to pointer)
					continue;
				}
				if (param_type->kind == TypeKind::Ptr && arg_type->kind != TypeKind::Ptr) {
					// Allow passing non-pointer to pointer parameter (implicit address-of)
					try {
						unify.unify(param_type->data.ptr.pointee, arg_type);
					} catch (const std::exception& ex) {
						report_type_error(node, ex.what());
					}
					continue;
				}
				if (is_numeric(param_type) && is_numeric(arg_type)) {
					// Allow numeric promotion (e.g., i32 -> i64/usize)
					continue;
				}
				try {
					unify.unify(fn_type->data.fn.params[i], arg_types[i]);
				} catch (const std::exception& ex) {
					report_type_error(node, ex.what());
				}
			}
			// Check vararg arguments
			if (fn_type->data.fn.vararg) {
				for (size_t i = expected; i < actual; i++) {
					TypePtr arg_type = unify.apply(arg_types[i]);
					if (!is_valid_vararg_type(arg_type)) {
						report_type_error(node, "cannot pass type '" + unify.pretty(arg_type) + "' to variadic function parameter");
					}
				}
			}
			return node_types[node] = fn_type->data.fn.ret;
		}

		if (resolved->kind == TypeKind::Ptr) {
			TypePtr pointee = unify.apply(resolved->data.ptr.pointee);
			TypePtr fn_type = nullptr;
			if (pointee->kind == TypeKind::Fn) {
				fn_type = pointee;
			} else {
				fn_type = Type::make_fn(arg_types, unify.fresh());
				try {
					unify.unify(pointee, fn_type);
				} catch (const std::exception& ex) {
					report_type_error(node, ex.what());
				}
			}
			size_t expected = fn_type->data.fn.params.size();
			if (!fn_type->data.fn.vararg && expected != actual) {
				report_semantic_error(node, "function call arity mismatch");
			}
			for (size_t i = 0; i < actual && i < expected; i++) {
				try {
					unify.unify(fn_type->data.fn.params[i], arg_types[i]);
				} catch (const std::exception& ex) {
					report_type_error(node, ex.what());
				}
			}
			return node_types[node] = fn_type->data.fn.ret;
		}

		if (resolved->kind == TypeKind::Var) {
			TypePtr fn_type = Type::make_fn(arg_types, unify.fresh());
			try {
				unify.unify(resolved, fn_type);
			} catch (const std::exception& ex) {
				report_type_error(node, ex.what());
			}
			return node_types[node] = fn_type->data.fn.ret;
		}

		// Constructor calls (Some/None/Ok/Err).
		if (node->data.call.func && node->data.call.func->type == AST_IDENTIFIER) {
			const char* name = node->data.call.func->data.identifier.name;
			if (name && strcmp(name, "Some") == 0) {
				TypePtr arg_t = nullptr;
				if (node->data.call.args && node->data.call.args->type == AST_EXPRESSION_LIST &&
					node->data.call.args->data.expression_list.expression_count == 1) {
					arg_t = check_expr(node->data.call.args->data.expression_list.expressions[0]);
				} else {
					report_semantic_error(node, "Some(...) expects one argument");
					arg_t = unify.fresh();
				}
				return node_types[node] = Type::make_app(Type::make_struct("Option"), {arg_t});
			}
			if (name && strcmp(name, "None") == 0) {
				return node_types[node] = Type::make_app(Type::make_struct("Option"), {unify.fresh()});
			}
		}

		report_type_error(node, "call of non-function");
		return node_types[node] = unify.fresh();
	}

	TypePtr check_global(ASTNode* node) {
		ASTNode* identifier = node->data.global_decl.identifier;
		ASTNode* type_node = node->data.global_decl.type;
		ASTNode* initializer = node->data.global_decl.initializer;

		TypePtr declared_type = type_node ? type_from_ast(type_node) : TypePtr(nullptr);
		TypePtr init_type = initializer ? check_expr(initializer) : builtin_void;

		if (declared_type && initializer) {
			try {
				unify.unify(declared_type, init_type);
			} catch (const std::exception& ex) {
				report_type_error(node, ex.what());
			}
		}

		TypePtr var_type = declared_type ? unify.apply(declared_type) : unify.apply(init_type);

		if (identifier && identifier->type == AST_IDENTIFIER) {
			const char* name = identifier->data.identifier.name;
			if (name) {
				if (!env.declare_value(name, var_type, identifier->mutability == MUTABILITY_MUTABLE, false)) {
					report_semantic_error(node, std::string("redefinition: ") + name);
				}
			}
		}

		return node_types[node] = var_type;
	}

	TypePtr check_function(ASTNode* node) {
		env.enter_scope();
		TypePtr fn_type = env.lookup_value(node->data.function.name)
							  ? env.lookup_value(node->data.function.name)->type
							  : register_function(node);
		auto saved_generics = generic_bindings;
		generic_bindings.clear();
		if (node->data.function.generic_params &&
			node->data.function.generic_params->type == AST_EXPRESSION_LIST) {
			int count = node->data.function.generic_params->data.expression_list.expression_count;
			const auto& ids = fn_type->data.fn.generic_param_ids;
			for (int i = 0; i < count; i++) {
				ASTNode* g = node->data.function.generic_params->data.expression_list.expressions[i];
				if (!g || g->type != AST_IDENTIFIER || !g->data.identifier.name) {
					continue;
				}
				int id = (i < (int)ids.size()) ? ids[i] : unify.fresh()->data.var.id;
				generic_bindings[g->data.identifier.name] = Type::make_var(id);
			}
		}

		if (node->data.function.is_extern) {
			generic_bindings = std::move(saved_generics);
			env.exit_scope();
			return node_types[node] = fn_type;
		}

		if (node->data.function.params && node->data.function.params->type == AST_EXPRESSION_LIST) {
			int count = node->data.function.params->data.expression_list.expression_count;
			for (int i = 0; i < count; i++) {
				ASTNode* param = node->data.function.params->data.expression_list.expressions[i];
				if (!param) {
					continue;
				}
				if (param->type == AST_ASSIGN && param->data.assign.left &&
					param->data.assign.left->type == AST_IDENTIFIER) {
					TypePtr ptype = type_from_ast(param->data.assign.right);
					env.declare_value(param->data.assign.left->data.identifier.name, ptype,
									  param->mutability == MUTABILITY_MUTABLE, false);
				} else if (param->type == AST_IDENTIFIER) {
					env.declare_value(param->data.identifier.name, unify.fresh(),
									  param->mutability == MUTABILITY_MUTABLE, false);
				}
			}
		}

		TypePtr body_type = check_block(node->data.function.body, false);
		warn_array_parameter_value_semantics(node);
		TypePtr ret_type = type_from_ast(node->data.function.return_type);
		if (!(node->data.function.is_extern && node->data.function.body == nullptr)) {
			TypePtr resolved_ret = unify.apply(ret_type);
			TypePtr resolved_body = unify.apply(body_type);
			/* For void functions, don't fail on implicit body type (e.g. last statement is a non-void call) */
			bool skip_unify = false;
			if (resolved_ret->kind == TypeKind::Void && resolved_body->kind != TypeKind::Void) {
				/* Check if the body has explicit return statements with values */
				bool has_explicit_return = find_return_node(node->data.function.body) != nullptr;
				if (!has_explicit_return) {
					skip_unify = true;
				}
			}
			if (!skip_unify) {
				try {
					unify.unify(ret_type, body_type);
				} catch (const std::exception& ex) {
					/* Try auto-deref: if body_type is Ptr[T] and ret_type is T, allow it */
					bool auto_deref_ok = false;
					if (resolved_body->kind == TypeKind::Ptr && resolved_body->data.ptr.pointee) {
						try {
							unify.unify(resolved_ret, resolved_body->data.ptr.pointee);
							auto_deref_ok = true;
						} catch (...) {}
					}
					if (!auto_deref_ok && resolved_ret->kind == TypeKind::Ptr && resolved_ret->data.ptr.pointee) {
						try {
							unify.unify(resolved_body, resolved_ret->data.ptr.pointee);
							auto_deref_ok = true;
						} catch (...) {}
					}
					if (!auto_deref_ok) {
						ASTNode* ret_node = find_return_node(node->data.function.body);
						if (ret_node && ret_node->data.return_stmt.expr) {
							report_type_error(ret_node->data.return_stmt.expr, ex.what());
						} else if (ret_node) {
							report_type_error(ret_node, ex.what());
						} else {
							report_type_error(node, ex.what());
						}
					}
				}
			}
		}
		generic_bindings = std::move(saved_generics);
		env.exit_scope();
		return node_types[node] = fn_type;
	}
};

}  // namespace

int vix_typecheck_program(ASTNode* root) {
	TypeChecker checker;
	checker.check_program(root);
	return checker.error_count == 0 ? 0 : 1;
}
