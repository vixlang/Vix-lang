#include "Types.h"
#include "../../include/ast.h"
#include "../../include/parser.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

void TypeHelper::setGenericTypeBindings(const std::map<std::string, Type*>& bindings) {
    genericTypeBindings = bindings;
}

void TypeHelper::clearGenericTypeBindings() {
    genericTypeBindings.clear();
}

void TypeHelper::registerStringVariable(const std::string& name) {
    stringVariables[name] = true;
}

bool TypeHelper::isStringVariable(const std::string& name) {
    return stringVariables.find(name) != stringVariables.end();
}

void TypeHelper::registerArrayType(const std::string& name, Type* elementType, int elementCount) {
    arrayTypes[name] = {elementType, elementCount};
}

std::pair<Type*, int>* TypeHelper::getArrayTypeInfo(const std::string& name) {
    auto it = arrayTypes.find(name);
    return it != arrayTypes.end() ? &it->second : nullptr;
}

void TypeHelper::registerVariableArraySize(const std::string& varName, int size) {
    variableArraySizes[varName] = size;
}

int TypeHelper::getVariableArraySize(const std::string& varName) {
    auto it = variableArraySizes.find(varName);
    return it != variableArraySizes.end() ? it->second : -1;
}

Type* TypeHelper::createArrayType(Type* elementType, int elementCount) {
    return ArrayType::get(elementType, elementCount);
}

Type* TypeHelper::getArrayElementTypeFromNode(ASTNode* node) {
    if (!node) return Type::getInt32Ty(context);
    if (node->type == AST_TYPE_LIST) {
        if (node->data.list_type.element_type)
            return getTypeFromTypeNode(node->data.list_type.element_type);
    }
    if (node->type == AST_TYPE_FIXED_SIZE_LIST) {
        if (node->data.fixed_size_list_type.element_type)
            return getTypeFromTypeNode(node->data.fixed_size_list_type.element_type);
    }
    return Type::getInt32Ty(context);
}

int TypeHelper::getArrayElementCountFromNode(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_TYPE_FIXED_SIZE_LIST)
        return (int)node->data.fixed_size_list_type.size;
    return 0;
}

void TypeHelper::registerStructType(const std::string& name, StructType* type,
                                     std::vector<std::pair<std::string, Type*>> fields) {
    structTypes[name] = type;
    structFields[name] = fields;
}

void TypeHelper::registerStructTemplate(const std::string& name, ASTNode* structDef) {
    structTemplates[name] = structDef;
}

ASTNode* TypeHelper::getStructTemplate(const std::string& name) {
    auto it = structTemplates.find(name);
    return it != structTemplates.end() ? it->second : nullptr;
}

StructType* TypeHelper::getStructType(const std::string& name) {
    auto it = structTypes.find(name);
    return it != structTypes.end() ? it->second : nullptr;
}

std::vector<std::pair<std::string, Type*>>* TypeHelper::getStructFields(const std::string& name) {
    auto it = structFields.find(name);
    return it != structFields.end() ? &it->second : nullptr;
}

int TypeHelper::getFieldIndex(const std::string& structName, const std::string& fieldName) {
    auto it = structFields.find(structName);
    if (it == structFields.end()) return -1;
    for (size_t i = 0; i < it->second.size(); i++) {
        if (it->second[i].first == fieldName) return i;
    }
    return -1;
}

std::string TypeHelper::typeNodeToToken(ASTNode* typeNode) const {
    auto sanitize = [](const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                out.push_back(c);
            } else {
                out.push_back('_');
            }
        }
        if (out.empty()) return std::string("unk");
        return out;
    };
    if (!typeNode) return "unk";
    switch (typeNode->type) {
        case AST_TYPE_INT32: return "i32";
        case AST_TYPE_INT64: return "i64";
        case AST_TYPE_INT8: return "i8";
        case AST_TYPE_FLOAT32: return "f32";
        case AST_TYPE_FLOAT64: return "f64";
        case AST_TYPE_STRING: return "str";
        case AST_TYPE_VOID: return "void";
        case AST_TYPE_POINTER: return "ptr";
        case AST_TYPE_LIST:
            return "list_" + typeNodeToToken(typeNode->data.list_type.element_type);
        case AST_TYPE_FIXED_SIZE_LIST:
            return "arr_" + typeNodeToToken(typeNode->data.fixed_size_list_type.element_type) +
                   "_" + std::to_string(typeNode->data.fixed_size_list_type.size);
        case AST_TYPE_APP: {
            std::string base = typeNodeToToken(typeNode->data.type_app.ctor);
            std::string out = base + "_app";
            if (typeNode->data.type_app.args && typeNode->data.type_app.args->type == AST_EXPRESSION_LIST) {
                for (int i = 0; i < typeNode->data.type_app.args->data.expression_list.expression_count; i++) {
                    out += "_" + typeNodeToToken(typeNode->data.type_app.args->data.expression_list.expressions[i]);
                }
            }
            return out;
        }
        case AST_IDENTIFIER:
            return typeNode->data.identifier.name ? sanitize(typeNode->data.identifier.name) : "id";
        default:
            return "unk";
    }
}

std::string TypeHelper::mangleStructInstanceName(const std::string& baseName, ASTNode* typeArgs) const {
    std::string name = baseName;
    name += "__g";
    if (!typeArgs || typeArgs->type != AST_EXPRESSION_LIST) return name;
    for (int i = 0; i < typeArgs->data.expression_list.expression_count; i++) {
        name += "_" + typeNodeToToken(typeArgs->data.expression_list.expressions[i]);
    }
    return name;
}

Type* TypeHelper::instantiateStructType(const std::string& baseName, ASTNode* typeArgs) {
    ASTNode* structDef = getStructTemplate(baseName);
    if (!structDef || structDef->type != AST_STRUCT_DEF) return nullptr;

    std::string mangledName = mangleStructInstanceName(baseName, typeArgs);
    if (StructType* existing = getStructType(mangledName)) return existing;

    // Cycle detection: if we're already instantiating this struct, return a pointer type
    // to break the infinite recursion (self-recursive generic struct)
    if (instantiating.count(mangledName)) {
        return PointerType::get(context, 0);
    }

    instantiating.insert(mangledName);

    ASTNode* genericParams = structDef->data.struct_def.generic_params;
    ASTNode* fields = structDef->data.struct_def.fields;
    if (!genericParams || genericParams->type != AST_EXPRESSION_LIST ||
        !fields || fields->type != AST_EXPRESSION_LIST ||
        !typeArgs || typeArgs->type != AST_EXPRESSION_LIST) {
        instantiating.erase(mangledName);
        return nullptr;
    }

    int paramCount = genericParams->data.expression_list.expression_count;
    int argCount = typeArgs->data.expression_list.expression_count;
    if (paramCount != argCount) {
        instantiating.erase(mangledName);
        return nullptr;
    }

    std::map<std::string, Type*> savedBindings = genericTypeBindings;
    for (int i = 0; i < paramCount; i++) {
        ASTNode* p = genericParams->data.expression_list.expressions[i];
        ASTNode* a = typeArgs->data.expression_list.expressions[i];
        if (!p || p->type != AST_IDENTIFIER || !p->data.identifier.name || !a) {
            genericTypeBindings = std::move(savedBindings);
            instantiating.erase(mangledName);
            return nullptr;
        }
        genericTypeBindings[p->data.identifier.name] = getTypeFromTypeNode(a);
    }

    StructType* structType = StructType::create(context, mangledName);
    std::vector<Type*> fieldTypes;
    std::vector<std::pair<std::string, Type*>> fieldInfo;
    int fieldCount = fields->data.expression_list.expression_count;
    for (int i = 0; i < fieldCount; i++) {
        ASTNode* field = fields->data.expression_list.expressions[i];
        if (!field || field->type != AST_ASSIGN) continue;
        ASTNode* left = field->data.assign.left;
        ASTNode* right = field->data.assign.right;
        if (!left || left->type != AST_IDENTIFIER || !left->data.identifier.name) continue;
        Type* fieldType = getTypeFromTypeNode(right);
        if (!fieldType) fieldType = Type::getInt32Ty(context);
        fieldTypes.push_back(fieldType);
        fieldInfo.push_back({left->data.identifier.name, fieldType});
    }

    genericTypeBindings = std::move(savedBindings);
    instantiating.erase(mangledName);
    if (fieldTypes.empty()) return nullptr;
    structType->setBody(fieldTypes, false);
    registerStructType(mangledName, structType, fieldInfo);
    return structType;
}

StructType* TypeHelper::inferStructTypeByFieldName(const std::string& fieldName, std::string* outStructName) {
    StructType* matchedType = nullptr;
    std::string matchedName;

    for (const auto& it : structFields) {
        const std::string& structName = it.first;
        const auto& fields = it.second;
        for (const auto& field : fields) {
            if (field.first == fieldName) {
                auto sit = structTypes.find(structName);
                if (sit == structTypes.end() || !sit->second) return nullptr;
                if (matchedType && matchedType != sit->second) return nullptr;
                matchedType = sit->second;
                matchedName = structName;
                break;
            }
        }
    }

    if (matchedType && outStructName) *outStructName = matchedName;
    return matchedType;
}

Type* TypeHelper::getLLVMType(ValueType type) {
    switch (type) {
        case ValueType::VOID:    return Type::getVoidTy(context);
        case ValueType::INT32:   return Type::getInt32Ty(context);
        case ValueType::INT64:   return Type::getInt64Ty(context);
        case ValueType::INT8:    return Type::getInt8Ty(context);
        case ValueType::FLOAT32: return Type::getFloatTy(context);
        case ValueType::FLOAT64: return Type::getDoubleTy(context);
        case ValueType::BOOL:    return Type::getInt1Ty(context);
        case ValueType::POINTER: return PointerType::get(context, 0);
        case ValueType::STRING:  return PointerType::get(context, 0);
        case ValueType::ARRAY:   return PointerType::get(context, 0);
        default:                 return Type::getVoidTy(context);
    }
}

ValueType TypeHelper::fromLLVMType(Type* type) {
    if (!type) return ValueType::VOID;
    if (type->isIntegerTy(32)) return ValueType::INT32;
    if (type->isIntegerTy(64)) return ValueType::INT64;
    if (type->isIntegerTy(8))  return ValueType::INT8;
    if (type->isIntegerTy(1))  return ValueType::BOOL;
    if (type->isFloatTy())     return ValueType::FLOAT32;
    if (type->isDoubleTy())    return ValueType::FLOAT64;
    if (type->isArrayTy())     return ValueType::ARRAY;
    if (type->isPointerTy()) {
        if (type == getLLVMType(ValueType::STRING)) return ValueType::STRING;
        return ValueType::POINTER;
    }
    if (type->isStructTy()) return ValueType::POINTER;
    return ValueType::VOID;
}

Type* TypeHelper::getTypeFromTypeNode(ASTNode* node) {
    if (!node) return Type::getInt32Ty(context);
    if (node->type == AST_TYPE_INT32) return Type::getInt32Ty(context);
    if (node->type == AST_TYPE_INT64) return Type::getInt64Ty(context);
    if (node->type == AST_TYPE_INT8) return Type::getInt8Ty(context);
    if (node->type == AST_TYPE_FLOAT32) return Type::getFloatTy(context);
    if (node->type == AST_TYPE_FLOAT64) return Type::getDoubleTy(context);
    if (node->type == AST_TYPE_STRING) return PointerType::get(context, 0);
    if (node->type == AST_TYPE_VOID) return Type::getVoidTy(context);
    if (node->type == AST_TYPE_POINTER) {
        if (node->data.pointer_type.element_type) {
            Type* elemType = getTypeFromTypeNode(node->data.pointer_type.element_type);
            return PointerType::get(context, 0);
        }
        return PointerType::get(context, 0);
    }
    if (node->type == AST_TYPE_LIST) {
        Type* elementType = getArrayElementTypeFromNode(node);
        return PointerType::get(context, 0);
    }
    if (node->type == AST_TYPE_FIXED_SIZE_LIST) {
        Type* elementType = getArrayElementTypeFromNode(node);
        int elementCount = getArrayElementCountFromNode(node);
        if (elementCount > 0) return createArrayType(elementType, elementCount);
        return PointerType::get(context, 0);
    }
    if (node->type == AST_TYPE_APP) {
        ASTNode* ctorNode = node->data.type_app.ctor;
        std::string baseName;
        if (ctorNode && ctorNode->type == AST_IDENTIFIER && ctorNode->data.identifier.name)
            baseName = ctorNode->data.identifier.name;
        if (baseName.empty()) return Type::getInt32Ty(context);
        if (StructType* structType = getStructType(baseName)) return structType;
        if (ASTNode* templateNode = getStructTemplate(baseName)) {
            (void)templateNode;
            if (Type* inst = instantiateStructType(baseName, node->data.type_app.args)) return inst;
        }
        if (vix_is_adt_definition(baseName.c_str())) {
            StructType* adtStructTy = StructType::get(context, {Type::getInt32Ty(context), PointerType::get(context, 0)});
            return PointerType::get(context, 0);
        }
        if (baseName == "Option" || baseName == "Result") {
            StructType* adtStructTy = StructType::get(context, {Type::getInt32Ty(context), PointerType::get(context, 0)});
            return PointerType::get(context, 0);
        }
        return Type::getInt32Ty(context);
    }
    if (node->type == AST_IDENTIFIER) {
        std::string typeName(node->data.identifier.name);
        auto git = genericTypeBindings.find(typeName);
        if (git != genericTypeBindings.end() && git->second) return git->second;
        if (typeName == "ptr") return PointerType::get(context, 0);
        if (typeName == "i8" || typeName == "u8" || typeName == "char") return Type::getInt8Ty(context);
        if (typeName == "i32") return Type::getInt32Ty(context);
        if (typeName == "i64") return Type::getInt64Ty(context);
        if (typeName == "f32") return Type::getFloatTy(context);
        if (typeName == "f64") return Type::getDoubleTy(context);
        if (typeName == "void") return Type::getVoidTy(context);
        StructType* st = getStructType(typeName);
        if (st) return st;
        auto* arrayInfo = getArrayTypeInfo(typeName);
        if (arrayInfo) return PointerType::get(context, 0);
    }
    return Type::getInt32Ty(context);
}

ValueType TypeHelper::fromTypeNode(ASTNode* node) {
    if (!node) return ValueType::INT32;
    switch (node->type) {
        case AST_TYPE_INT32:   return ValueType::INT32;
        case AST_TYPE_INT64:   return ValueType::INT64;
        case AST_TYPE_INT8:    return ValueType::INT8;
        case AST_TYPE_FLOAT32: return ValueType::FLOAT32;
        case AST_TYPE_FLOAT64: return ValueType::FLOAT64;
        case AST_TYPE_STRING:  return ValueType::STRING;
        case AST_TYPE_VOID:    return ValueType::VOID;
        case AST_TYPE_POINTER: return ValueType::POINTER;
        case AST_TYPE_LIST:    return ValueType::ARRAY;
        case AST_TYPE_FIXED_SIZE_LIST: return ValueType::ARRAY;
        case AST_TYPE_APP:     return ValueType::POINTER;
        case AST_IDENTIFIER: {
            if (!node->data.identifier.name) return ValueType::INT32;
            std::string typeName(node->data.identifier.name);
            if (typeName == "ptr") return ValueType::POINTER;
            if (typeName == "i8" || typeName == "u8" || typeName == "char") return ValueType::INT8;
            if (typeName == "i32") return ValueType::INT32;
            if (typeName == "i64") return ValueType::INT64;
            if (typeName == "f32") return ValueType::FLOAT32;
            if (typeName == "f64") return ValueType::FLOAT64;
            if (typeName == "void") return ValueType::VOID;
            return ValueType::INT32;
        }
        default: return ValueType::INT32;
    }
}

ValueType TypeHelper::getValueTypeFromType(Type* type) {
    return fromLLVMType(type);
}

std::pair<ValueType, ValueType> TypeHelper::promoteTypes(ValueType left, ValueType right) {
    int leftRank = getTypeRank(left);
    int rightRank = getTypeRank(right);
    if (leftRank > rightRank) return {left, promoteTo(right, left)};
    else if (rightRank > leftRank) return {promoteTo(left, right), right};
    return {left, right};
}

ValueType TypeHelper::promoteTo(ValueType from, ValueType to) {
    if (from == to) return from;
    if (from == ValueType::BOOL || from == ValueType::INT8 || from == ValueType::INT32) {
        if (to == ValueType::INT64 || to == ValueType::FLOAT32 || to == ValueType::FLOAT64) return to;
    }
    if (from == ValueType::INT64) {
        if (to == ValueType::FLOAT32 || to == ValueType::FLOAT64) return to;
    }
    if (from == ValueType::FLOAT32 && to == ValueType::FLOAT64) return to;
    return from;
}

Value* TypeHelper::castValue(IRBuilder<>& builder, Value* val, ValueType from, ValueType to) {
    if (from == to) return val;

    auto isIntegerLike = [](ValueType t) {
        return t == ValueType::BOOL || t == ValueType::INT8 || t == ValueType::INT32 || t == ValueType::INT64;
    };

    if (isIntegerLike(from) && isIntegerLike(to)) {
        bool fromSigned = (from != ValueType::BOOL);
        return builder.CreateIntCast(val, getLLVMType(to), fromSigned, "icast");
    }

    if (from == ValueType::BOOL && to == ValueType::INT32)
        return builder.CreateZExt(val, Type::getInt32Ty(context), "zext");
    if (from == ValueType::INT32 && to == ValueType::INT64)
        return builder.CreateSExt(val, Type::getInt64Ty(context), "sext");
    if (from == ValueType::INT8 && to == ValueType::INT32)
        return builder.CreateSExt(val, Type::getInt32Ty(context), "sext8");
    if (from == ValueType::INT8 && to == ValueType::INT64)
        return builder.CreateSExt(val, Type::getInt64Ty(context), "sext64");
    if (from == ValueType::BOOL && to == ValueType::INT64)
        return builder.CreateZExt(val, Type::getInt64Ty(context), "zext64");

    if ((from == ValueType::INT32 || from == ValueType::INT64 || from == ValueType::INT8 || from == ValueType::BOOL)
        && (to == ValueType::FLOAT32 || to == ValueType::FLOAT64)) {
        if (to == ValueType::FLOAT32)
            return builder.CreateSIToFP(val, Type::getFloatTy(context), "itof32");
        else
            return builder.CreateSIToFP(val, Type::getDoubleTy(context), "itof64");
    }

    if (from == ValueType::FLOAT32 && to == ValueType::FLOAT64)
        return builder.CreateFPExt(val, Type::getDoubleTy(context), "fext32to64");
    if (from == ValueType::FLOAT64 && to == ValueType::FLOAT32)
        return builder.CreateFPTrunc(val, Type::getFloatTy(context), "ftrunc64to32");

    if (from == ValueType::FLOAT32 && (to == ValueType::INT32 || to == ValueType::INT64 || to == ValueType::INT8))
        return builder.CreateFPToSI(val, getLLVMType(to), "ftoi");
    if (from == ValueType::FLOAT64 && (to == ValueType::INT32 || to == ValueType::INT64 || to == ValueType::INT8))
        return builder.CreateFPToSI(val, getLLVMType(to), "ftoi");

    if ((from == ValueType::INT32 || from == ValueType::INT64 || from == ValueType::INT8 || from == ValueType::BOOL) &&
        (to == ValueType::POINTER || to == ValueType::STRING)) {
        Value* intVal = val;
        if (!intVal->getType()->isIntegerTy()) {
            if (intVal->getType()->isPointerTy())
                return builder.CreateBitCast(intVal, PointerType::get(context, 0), "ptr_to_ptr");
            return intVal;
        }
        if (!intVal->getType()->isIntegerTy(64))
            intVal = builder.CreateIntCast(intVal, Type::getInt64Ty(context), true, "int_to_ptr_int64");
        return builder.CreateIntToPtr(intVal, PointerType::get(context, 0), "int_to_ptr");
    }

    if (from == ValueType::POINTER && (to == ValueType::INT64 || to == ValueType::INT32)) {
        Value* intVal = builder.CreatePtrToInt(val, Type::getInt64Ty(context), "ptr_to_int64");
        if (to == ValueType::INT32)
            return builder.CreateTrunc(intVal, Type::getInt32Ty(context), "ptr_to_int32");
        return intVal;
    }

    if ((from == ValueType::POINTER && to == ValueType::STRING) ||
        (from == ValueType::STRING && to == ValueType::POINTER))
        return val;

    if (from == ValueType::POINTER && to == ValueType::POINTER && val->getType()->isPointerTy())
        return builder.CreateBitCast(val, PointerType::get(context, 0), "ptr_cast");

    return val;
}

int TypeHelper::getTypeRank(ValueType type) {
    switch (type) {
        case ValueType::BOOL:    return 0;
        case ValueType::INT8:    return 1;
        case ValueType::INT32:   return 2;
        case ValueType::INT64:   return 3;
        case ValueType::FLOAT32: return 4;
        case ValueType::FLOAT64: return 5;
        default:                 return -1;
    }
}
