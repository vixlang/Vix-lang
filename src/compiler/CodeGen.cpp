/*
vix0.0.1 released!
*/
#include "../../include/codegen.h"
#include "../../include/ast.h"
#include "../../include/parser.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/IR/Verifier.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <stdio.h>
#include <map>
#include <string>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>

using namespace llvm;

extern "C" const char* current_input_filename;
extern "C" void report_semantic_error_with_location(const char* message, const char* filename, int line);

static bool isBuiltinUnionCtorName(const std::string& name) {
    return name == "Some" || name == "None" || name == "Ok" || name == "Err";
}

static bool isRegisteredUnionCtorName(const std::string& name) {
    return vix_adt_ctor_base_name(name.c_str()) != nullptr;
}

static int32_t ctorTagValue(const std::string& name) {
    return static_cast<int32_t>(std::hash<std::string>{}(name) & 0x7fffffff);
}

static std::string g_vix_target_triple;

struct PrintfFormatSpec {
    char conv = 0;
    int length = 0; // 0: default, 1: l, 2: ll
};

static std::vector<PrintfFormatSpec> parsePrintfFormatSpecs(const std::string& fmt) {
    std::vector<PrintfFormatSpec> specs;
    size_t i = 0;
    while (i < fmt.size()) {
        if (fmt[i] != '%') {
            i++;
            continue;
        }

        i++;
        if (i < fmt.size() && fmt[i] == '%') {
            i++;
            continue;
        }

        while (i < fmt.size() &&
               (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#' || fmt[i] == '0')) {
            i++;
        }
        while (i < fmt.size() && (fmt[i] >= '0' && fmt[i] <= '9')) {
            i++;
        }
        if (i < fmt.size() && fmt[i] == '.') {
            i++;
            while (i < fmt.size() && (fmt[i] >= '0' && fmt[i] <= '9')) {
                i++;
            }
        }

        int length = 0;
        if (i < fmt.size() && fmt[i] == 'l') {
            length = 1;
            i++;
            if (i < fmt.size() && fmt[i] == 'l') {
                length = 2;
                i++;
            }
        }

        if (i < fmt.size()) {
            PrintfFormatSpec spec;
            spec.conv = fmt[i];
            spec.length = length;
            specs.push_back(spec);
            i++;
        }
    }
    return specs;
}

struct SymbolAttr {
    bool exported = false;
    std::string section;
};

struct SourceAttrInfo {
    bool noMain = false;
    bool noStd = false;
    std::map<std::string, SymbolAttr> functionAttrs;
    std::map<std::string, SymbolAttr> constAttrs;
};

static std::string trimCopy(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) start++;
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) end--;
    return s.substr(start, end - start);
}

static SourceAttrInfo parseSourceAttributes(const char* filePath) {
    SourceAttrInfo info;
    if (!filePath) return info;

    std::ifstream in(filePath);
    if (!in.is_open()) return info;

    SymbolAttr pending;
    bool hasPending = false;
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trimCopy(line);
        if (t.empty()) continue;

        if (t == "#[no_main]") { info.noMain = true; continue; }
        if (t == "#[no_std]") { info.noStd = true; continue; }
        if (t == "#[export]") { pending.exported = true; hasPending = true; continue; }

        if (t.rfind("#[link_section", 0) == 0) {
            size_t q1 = t.find('"');
            size_t q2 = t.rfind('"');
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
                pending.section = t.substr(q1 + 1, q2 - q1 - 1);
                hasPending = true;
            }
            continue;
        }

        if (t.rfind("pub fn ", 0) == 0 || t.rfind("fn ", 0) == 0) {
            size_t fnPos = t.rfind("fn ");
            size_t nameStart = fnPos + 3;
            size_t nameEnd = t.find('(', nameStart);
            if (nameEnd != std::string::npos && hasPending) {
                std::string name = trimCopy(t.substr(nameStart, nameEnd - nameStart));
                if (!name.empty()) info.functionAttrs[name] = pending;
            }
            pending = SymbolAttr();
            hasPending = false;
            continue;
        }

        if (t.rfind("const ", 0) == 0) {
            size_t nameStart = 6;
            size_t nameEnd = t.find_first_of(" :={", nameStart);
            if (nameEnd != std::string::npos && hasPending) {
                std::string name = trimCopy(t.substr(nameStart, nameEnd - nameStart));
                if (!name.empty()) info.constAttrs[name] = pending;
            }
            pending = SymbolAttr();
            hasPending = false;
            continue;
        }
    }

    return info;
}

static bool isVixDebugEnabled() {//通过环境变量控制调试输出
    const char* value = std::getenv("VIX_DEBUG");
    if (!value || value[0] == '\0') return false;
    return strcmp(value, "0") != 0 && strcmp(value, "false") != 0 && strcmp(value, "off") != 0;
}

static raw_ostream& vixDebugStream() {
    return isVixDebugEnabled() ? llvm::errs() : llvm::nulls();
}

#define VIX_DEBUG_LOG vixDebugStream()

// ==================== TYPES ====================
enum class ValueType {
    VOID,
    INT32,
    INT64,
    INT8,
    FLOAT32,
    FLOAT64,
    BOOL,
    POINTER,
    STRING,
    ARRAY
};

// ==================== INIT!!! ====================
void initTarget() {
    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmParsers();
    InitializeAllAsmPrinters();
}

// ==================== ScopeManager ====================
class ScopeManager {
private:
    std::vector<std::map<std::string, AllocaInst*>> scopes;
    Function* currentFunction;
    
public:
    ScopeManager() : currentFunction(nullptr) { enterScope(); }
    
    void enterScope() { scopes.push_back({}); }
    
    void exitScope() {
        if (scopes.size() > 1) scopes.pop_back();
    }
    
    void setCurrentFunction(Function* func) { currentFunction = func; }
    
    Function* getCurrentFunction() { return currentFunction; }
    
    void defineVariable(const std::string& name, AllocaInst* alloc) {
        if (!scopes.empty()) scopes.back()[name] = alloc;
    }
    
    AllocaInst* findVariable(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        return nullptr;
    }
    
    void clear() { scopes.clear(); enterScope(); }
};

// ==================== Type Helper ====================
class TypeHelper {
private:
    LLVMContext& context;
    std::map<std::string, StructType*> structTypes;
    std::map<std::string, std::vector<std::pair<std::string, Type*>>> structFields;
    std::map<std::string, ASTNode*> structTemplates;
    std::map<std::string, std::pair<Type*, int>> arrayTypes;
    std::map<std::string, int> variableArraySizes;
    std::map<std::string, bool> stringVariables;//标记字符串变量
    std::map<std::string, Type*> genericTypeBindings;
    
public:
    TypeHelper(LLVMContext& ctx) : context(ctx) {}
    void setGenericTypeBindings(const std::map<std::string, Type*>& bindings) {
        genericTypeBindings = bindings;
    }
    void clearGenericTypeBindings() {
        genericTypeBindings.clear();
    }
    void registerStringVariable(const std::string& name) {
        stringVariables[name] = true;
    }
    bool isStringVariable(const std::string& name) {
        return stringVariables.find(name) != stringVariables.end();
    }
    
    void registerArrayType(const std::string& name, Type* elementType, int elementCount) {
        arrayTypes[name] = {elementType, elementCount};
        if (elementType->isIntegerTy(8)) {
            stringVariables[name] = true;
        }
    }
    
    std::pair<Type*, int>* getArrayTypeInfo(const std::string& name) {
        auto it = arrayTypes.find(name);
        return it != arrayTypes.end() ? &it->second : nullptr;
    }
    
    void registerVariableArraySize(const std::string& varName, int size) {
        variableArraySizes[varName] = size;
    }
    
    int getVariableArraySize(const std::string& varName) {
        auto it = variableArraySizes.find(varName);
        return it != variableArraySizes.end() ? it->second : -1;
    }
    
    Type* createArrayType(Type* elementType, int elementCount) {
        return ArrayType::get(elementType, elementCount);
    }
    
    Type* getArrayElementTypeFromNode(ASTNode* node) {
        if (!node) return Type::getInt32Ty(context);
        
        if (node->type == AST_TYPE_LIST) {
            if (node->data.list_type.element_type) {
                return getTypeFromTypeNode(node->data.list_type.element_type);
            }
        }
        
        if (node->type == AST_TYPE_FIXED_SIZE_LIST) {
            if (node->data.fixed_size_list_type.element_type) {
                return getTypeFromTypeNode(node->data.fixed_size_list_type.element_type);
            }
        }
        
        return Type::getInt32Ty(context);
    }
    
    int getArrayElementCountFromNode(ASTNode* node) {
        if (!node) return 0;
        
        if (node->type == AST_TYPE_FIXED_SIZE_LIST) {
            return (int)node->data.fixed_size_list_type.size;
        }
        
        return 0;
    }
    
    void registerStructType(const std::string& name, StructType* type, 
                           std::vector<std::pair<std::string, Type*>> fields) {
        structTypes[name] = type;
        structFields[name] = fields;
    }

    void registerStructTemplate(const std::string& name, ASTNode* structDef) {
        structTemplates[name] = structDef;
    }

    ASTNode* getStructTemplate(const std::string& name) {
        auto it = structTemplates.find(name);
        return it != structTemplates.end() ? it->second : nullptr;
    }
    
    StructType* getStructType(const std::string& name) {
        auto it = structTypes.find(name);
        return it != structTypes.end() ? it->second : nullptr;
    }
    
    std::vector<std::pair<std::string, Type*>>* getStructFields(const std::string& name) {
        auto it = structFields.find(name);
        return it != structFields.end() ? &it->second : nullptr;
    }
    
    int getFieldIndex(const std::string& structName, const std::string& fieldName) {
        auto it = structFields.find(structName);
        if (it == structFields.end()) return -1;
        for (size_t i = 0; i < it->second.size(); i++) {
            if (it->second[i].first == fieldName) return i;
        }
        return -1;
    }

    std::string typeNodeToToken(ASTNode* typeNode) const {
        auto sanitize = [](const std::string& raw) {
            std::string out;
            out.reserve(raw.size());
            for (char c : raw) {
                if ((c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    c == '_') {
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
            case AST_TYPE_LIST: {
                return "list_" + typeNodeToToken(typeNode->data.list_type.element_type);
            }
            case AST_TYPE_FIXED_SIZE_LIST: {
                return "arr_" + typeNodeToToken(typeNode->data.fixed_size_list_type.element_type) +
                       "_" + std::to_string(typeNode->data.fixed_size_list_type.size);
            }
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

    std::string mangleStructInstanceName(const std::string& baseName, ASTNode* typeArgs) const {
        std::string name = baseName;
        name += "__g";
        if (!typeArgs || typeArgs->type != AST_EXPRESSION_LIST) return name;
        for (int i = 0; i < typeArgs->data.expression_list.expression_count; i++) {
            name += "_" + typeNodeToToken(typeArgs->data.expression_list.expressions[i]);
        }
        return name;
    }

    Type* instantiateStructType(const std::string& baseName, ASTNode* typeArgs) {
        ASTNode* structDef = getStructTemplate(baseName);
        if (!structDef || structDef->type != AST_STRUCT_DEF) return nullptr;

        std::string mangledName = mangleStructInstanceName(baseName, typeArgs);
        if (StructType* existing = getStructType(mangledName)) {
            return existing;
        }

        ASTNode* genericParams = structDef->data.struct_def.generic_params;
        ASTNode* fields = structDef->data.struct_def.fields;
        if (!genericParams || genericParams->type != AST_EXPRESSION_LIST ||
            !fields || fields->type != AST_EXPRESSION_LIST ||
            !typeArgs || typeArgs->type != AST_EXPRESSION_LIST) {
            return nullptr;
        }

        int paramCount = genericParams->data.expression_list.expression_count;
        int argCount = typeArgs->data.expression_list.expression_count;
        if (paramCount != argCount) return nullptr;

        std::map<std::string, Type*> savedBindings = genericTypeBindings;
        for (int i = 0; i < paramCount; i++) {
            ASTNode* p = genericParams->data.expression_list.expressions[i];
            ASTNode* a = typeArgs->data.expression_list.expressions[i];
            if (!p || p->type != AST_IDENTIFIER || !p->data.identifier.name || !a) {
                genericTypeBindings = std::move(savedBindings);
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
        if (fieldTypes.empty()) return nullptr;
        structType->setBody(fieldTypes, false);
        registerStructType(mangledName, structType, fieldInfo);
        return structType;
    }

    StructType* inferStructTypeByFieldName(const std::string& fieldName, std::string* outStructName = nullptr) {
        StructType* matchedType = nullptr;
        std::string matchedName;

        for (const auto& it : structFields) {
            const std::string& structName = it.first;
            const auto& fields = it.second;
            for (const auto& field : fields) {
                if (field.first == fieldName) {
                    auto sit = structTypes.find(structName);
                    if (sit == structTypes.end() || !sit->second) {
                        return nullptr;
                    }
                    if (matchedType && matchedType != sit->second) {
                        return nullptr;
                    }
                    matchedType = sit->second;
                    matchedName = structName;
                    break;
                }
            }
        }

        if (matchedType && outStructName) {
            *outStructName = matchedName;
        }
        return matchedType;
    }
    
    Type* getLLVMType(ValueType type) {
        switch (type) {
            case ValueType::VOID:    return Type::getVoidTy(context);
            case ValueType::INT32:   return Type::getInt32Ty(context);
            case ValueType::INT64:   return Type::getInt64Ty(context);
            case ValueType::INT8:    return Type::getInt8Ty(context);
            case ValueType::FLOAT32: return Type::getFloatTy(context);
            case ValueType::FLOAT64: return Type::getDoubleTy(context);
            case ValueType::BOOL:    return Type::getInt1Ty(context);
            case ValueType::POINTER: return PointerType::getUnqual(Type::getInt8Ty(context));
            case ValueType::STRING:  return PointerType::getUnqual(Type::getInt8Ty(context));
            case ValueType::ARRAY:   return PointerType::getUnqual(Type::getInt8Ty(context));
            default:                 return Type::getVoidTy(context);
        }
    }
    
    ValueType fromLLVMType(Type* type) {
        if (!type) return ValueType::VOID;
        
        if (type->isIntegerTy(32)) return ValueType::INT32;
        if (type->isIntegerTy(64)) return ValueType::INT64;
        if (type->isIntegerTy(8))  return ValueType::INT8;
        if (type->isIntegerTy(1))  return ValueType::BOOL;
        if (type->isFloatTy())     return ValueType::FLOAT32;
        if (type->isDoubleTy())    return ValueType::FLOAT64;
        if (type->isArrayTy())     return ValueType::ARRAY;
        
        if (type->isPointerTy()) {
            if (type == getLLVMType(ValueType::STRING)) {
                return ValueType::STRING;
            }
            return ValueType::POINTER;
        }
        
        if (type->isStructTy()) {
            return ValueType::POINTER;
        }
        
        return ValueType::VOID;
    }
    
    Type* getTypeFromTypeNode(ASTNode* node) {
        if (!node) return Type::getInt32Ty(context);
        
        if (node->type == AST_TYPE_INT32) return Type::getInt32Ty(context);
        if (node->type == AST_TYPE_INT64) return Type::getInt64Ty(context);
        if (node->type == AST_TYPE_INT8) return Type::getInt8Ty(context);
        if (node->type == AST_TYPE_FLOAT32) return Type::getFloatTy(context);
        if (node->type == AST_TYPE_FLOAT64) return Type::getDoubleTy(context);
        if (node->type == AST_TYPE_STRING) return PointerType::getUnqual(Type::getInt8Ty(context));
        if (node->type == AST_TYPE_VOID) return Type::getVoidTy(context);
        if (node->type == AST_TYPE_POINTER) {
            if (node->data.pointer_type.element_type) {
                Type* elemType = getTypeFromTypeNode(node->data.pointer_type.element_type);
                return PointerType::getUnqual(elemType ? elemType : Type::getInt8Ty(context));
            }
            return PointerType::getUnqual(Type::getInt8Ty(context));
        }
        
        if (node->type == AST_TYPE_LIST) {
            Type* elementType = getArrayElementTypeFromNode(node);
            return PointerType::getUnqual(elementType);
        }
        
        if (node->type == AST_TYPE_FIXED_SIZE_LIST) {
            Type* elementType = getArrayElementTypeFromNode(node);
            int elementCount = getArrayElementCountFromNode(node);
            if (elementCount > 0) {
                return createArrayType(elementType, elementCount);
            }
            return PointerType::getUnqual(elementType);
        }

        if (node->type == AST_TYPE_APP) {
            ASTNode* ctorNode = node->data.type_app.ctor;
            std::string baseName;
            if (ctorNode && ctorNode->type == AST_IDENTIFIER && ctorNode->data.identifier.name) {
                baseName = ctorNode->data.identifier.name;
            }
            if (baseName.empty()) {
                return Type::getInt32Ty(context);
            }

            if (StructType* structType = getStructType(baseName)) {
                return structType;
            }
            if (ASTNode* templateNode = getStructTemplate(baseName)) {
                (void)templateNode;
                if (Type* inst = instantiateStructType(baseName, node->data.type_app.args)) {
                    return inst;
                }
            }
            if (vix_is_adt_definition(baseName.c_str())) {
                StructType* adtStructTy = StructType::get(context, {Type::getInt32Ty(context), PointerType::getUnqual(Type::getInt8Ty(context))});
                return PointerType::getUnqual(adtStructTy);
            }
            if (baseName == "Option" || baseName == "Result") {
                StructType* adtStructTy = StructType::get(context, {Type::getInt32Ty(context), PointerType::getUnqual(Type::getInt8Ty(context))});
                return PointerType::getUnqual(adtStructTy);
            }
            return Type::getInt32Ty(context);
        }
        
        if (node->type == AST_IDENTIFIER) {
            std::string typeName(node->data.identifier.name);
            auto git = genericTypeBindings.find(typeName);
            if (git != genericTypeBindings.end() && git->second) {
                return git->second;
            }
            if (typeName == "ptr") return PointerType::getUnqual(Type::getInt8Ty(context));
            if (typeName == "str") return PointerType::getUnqual(Type::getInt8Ty(context));
            if (typeName == "i8" || typeName == "u8" || typeName == "char") return Type::getInt8Ty(context);
            if (typeName == "i32") return Type::getInt32Ty(context);
            if (typeName == "i64") return Type::getInt64Ty(context);
            if (typeName == "f32") return Type::getFloatTy(context);
            if (typeName == "f64") return Type::getDoubleTy(context);
            if (typeName == "void") return Type::getVoidTy(context);
            StructType* st = getStructType(typeName);
            if (st) return st;
            
            auto* arrayInfo = getArrayTypeInfo(typeName);
            if (arrayInfo) {
                return PointerType::getUnqual(arrayInfo->first);
            }
        }
        
        return Type::getInt32Ty(context);
    }
    
    ValueType fromTypeNode(ASTNode* node) {
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
            case AST_TYPE_APP:             return ValueType::POINTER;
            case AST_IDENTIFIER: {
                if (!node->data.identifier.name) return ValueType::INT32;
                std::string typeName(node->data.identifier.name);
                if (typeName == "ptr") return ValueType::POINTER;
                if (typeName == "str") return ValueType::STRING;
                if (typeName == "i8" || typeName == "u8" || typeName == "char") return ValueType::INT8;
                if (typeName == "i32") return ValueType::INT32;
                if (typeName == "i64") return ValueType::INT64;
                if (typeName == "f32") return ValueType::FLOAT32;
                if (typeName == "f64") return ValueType::FLOAT64;
                if (typeName == "void") return ValueType::VOID;
                return ValueType::INT32;
            }
            default:               return ValueType::INT32;
        }
    }
    
    ValueType getValueTypeFromType(Type* type) {
        return fromLLVMType(type);
    }
    
    std::pair<ValueType, ValueType> promoteTypes(ValueType left, ValueType right) {
        int leftRank = getTypeRank(left);
        int rightRank = getTypeRank(right);
        if (leftRank > rightRank) {
            return {left, promoteTo(right, left)};
        } else if (rightRank > leftRank) {
            return {promoteTo(left, right), right};
        }
        return {left, right};
    }
    
    ValueType promoteTo(ValueType from, ValueType to) {
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
    
    Value* castValue(IRBuilder<>& builder, Value* val, ValueType from, ValueType to) {
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
                if (intVal->getType()->isPointerTy()) {
                    return builder.CreateBitCast(intVal, PointerType::getUnqual(Type::getInt8Ty(context)), "ptr_to_ptr");
                }
                return intVal;
            }
            if (!intVal->getType()->isIntegerTy(64)) {
                intVal = builder.CreateIntCast(intVal, Type::getInt64Ty(context), true, "int_to_ptr_int64");
            }
            return builder.CreateIntToPtr(intVal, PointerType::getUnqual(Type::getInt8Ty(context)), "int_to_ptr");
        }

        if (from == ValueType::POINTER && (to == ValueType::INT64 || to == ValueType::INT32)) {
            Value* intVal = builder.CreatePtrToInt(val, Type::getInt64Ty(context), "ptr_to_int64");
            if (to == ValueType::INT32) {
                return builder.CreateTrunc(intVal, Type::getInt32Ty(context), "ptr_to_int32");
            }
            return intVal;
        }
        
        if ((from == ValueType::POINTER && to == ValueType::STRING) ||
            (from == ValueType::STRING && to == ValueType::POINTER))
            return val;

        if (from == ValueType::POINTER && to == ValueType::POINTER && val->getType()->isPointerTy()) {
            return builder.CreateBitCast(val, PointerType::getUnqual(Type::getInt8Ty(context)), "ptr_cast");
        }
        
        return val;
    }
    
private:
    int getTypeRank(ValueType type) {
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
};

// ==================== LLVM EMITTER ====================
class LLVMCodeGenerator {
public:
    struct VisitResult;

private:
    LLVMContext context;
    IRBuilder<> builder;
    std::unique_ptr<Module> module;
    ScopeManager scopeManager;
    TypeHelper typeHelper;
    Function* printfFunction;
    Function* strlenFunction;
    bool isGlobalScope;
    bool mainFunctionCreated;
    SourceAttrInfo sourceAttrs;
    std::map<std::string, std::vector<int>> functionArrayParamPositions;
    std::map<std::string, StructType*> functionSRetResultTypesByName;
    std::map<Function*, StructType*> functionSRetResultTypesByFunc;
    std::map<std::string, Type*> functionArrayReturnElementTypes;
    std::map<std::string, ASTNode*> genericFunctionTemplates;
    std::map<std::string, int> genericFunctionArity;
    std::map<std::string, Type*> activeGenericTypeBindings;
    std::map<const Value*, Type*> pointerElementHints;
    std::map<std::string, int> memberArrayLengthHints;
    std::map<std::string, int> memberNestedArrayLengthHints;
    std::vector<BasicBlock*> loopBreakTargets;
    std::vector<BasicBlock*> loopContinueTargets;

    void reportCodegenSemanticError(ASTNode* node, const std::string& message) {
        const char* filename = (node && node->source_file) ? node->source_file :
            (current_input_filename ? current_input_filename : "unknown");
        int line = (node && node->location.first_line > 0) ? node->location.first_line : 1;
        report_semantic_error_with_location(message.c_str(), filename, line);
    }

    bool usesStructSRet(Function* func) const {
        if (!func) return false;
        return functionSRetResultTypesByFunc.find(func) != functionSRetResultTypesByFunc.end();
    }

    StructType* getStructSRetType(Function* func) const {
        if (!func) return nullptr;
        auto it = functionSRetResultTypesByFunc.find(func);
        return it != functionSRetResultTypesByFunc.end() ? it->second : nullptr;
    }

    void registerStructSRetFunction(const std::string& funcName, Function* func, StructType* structType) {
        if (!func || !structType) return;
        functionSRetResultTypesByName[funcName] = structType;
        functionSRetResultTypesByFunc[func] = structType;
    }

    static std::string sanitizeTypeToken(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw) {
            if ((c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '_') {
                out.push_back(c);
            } else {
                out.push_back('_');
            }
        }
        if (out.empty()) return "unk";
        return out;
    }

    std::string typeNodeToMangleToken(ASTNode* typeNode) const {
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
            case AST_TYPE_LIST: {
                std::string elem = typeNodeToMangleToken(typeNode->data.list_type.element_type);
                return "list_" + sanitizeTypeToken(elem);
            }
            case AST_TYPE_FIXED_SIZE_LIST: {
                std::string elem = typeNodeToMangleToken(typeNode->data.fixed_size_list_type.element_type);
                return "arr_" + sanitizeTypeToken(elem) + "_" + std::to_string(typeNode->data.fixed_size_list_type.size);
            }
            case AST_IDENTIFIER:
                if (typeNode->data.identifier.name) return sanitizeTypeToken(typeNode->data.identifier.name);
                return "id";
            default:
                return "unk";
        }
    }

    std::string mangleGenericFunctionName(const std::string& baseName, ASTNode* typeArgs) const {
        std::string name = baseName;
        name += "__g";
        if (!typeArgs || typeArgs->type != AST_EXPRESSION_LIST) return name;
        for (int i = 0; i < typeArgs->data.expression_list.expression_count; i++) {
            ASTNode* arg = typeArgs->data.expression_list.expressions[i];
            name += "_";
            name += typeNodeToMangleToken(arg);
        }
        return name;
    }

    bool bindGenericTypeArgs(ASTNode* fnNode, ASTNode* typeArgs, std::map<std::string, Type*>& outBindings) {
        outBindings.clear();
        if (!fnNode || fnNode->type != AST_FUNCTION) return false;
        ASTNode* genericParams = fnNode->data.function.generic_params;
        if (!genericParams || genericParams->type != AST_EXPRESSION_LIST) return false;
        if (!typeArgs) {
            int paramCount = genericParams->data.expression_list.expression_count;
            for (int i = 0; i < paramCount; i++) {
                ASTNode* p = genericParams->data.expression_list.expressions[i];
                if (!p || p->type != AST_IDENTIFIER || !p->data.identifier.name) {
                    return false;
                }
                outBindings[p->data.identifier.name] = Type::getInt32Ty(context);
            }
            return true;
        }
        if (typeArgs->type != AST_EXPRESSION_LIST) return false;

        int paramCount = genericParams->data.expression_list.expression_count;
        int argCount = typeArgs->data.expression_list.expression_count;
        if (paramCount != argCount) return false;

        for (int i = 0; i < paramCount; i++) {
            ASTNode* p = genericParams->data.expression_list.expressions[i];
            ASTNode* a = typeArgs->data.expression_list.expressions[i];
            if (!p || p->type != AST_IDENTIFIER || !p->data.identifier.name || !a) {
                return false;
            }
            Type* concrete = typeHelper.getTypeFromTypeNode(a);
            outBindings[p->data.identifier.name] = concrete;
        }

        return true;
    }

    Function* instantiateGenericFunction(const std::string& baseName, ASTNode* typeArgs) {
        auto fit = genericFunctionTemplates.find(baseName);
        if (fit == genericFunctionTemplates.end()) {
            return nullptr;
        }

        std::string mangledName = mangleGenericFunctionName(baseName, typeArgs);
        if (Function* existing = module->getFunction(mangledName)) {
            return existing;
        }

        std::map<std::string, Type*> bindings;
        if (!bindGenericTypeArgs(fit->second, typeArgs, bindings)) {
            llvm::errs() << "Error: Failed to bind generic type arguments for function '" << baseName << "'\n";
            return nullptr;
        }

        auto oldBindings = activeGenericTypeBindings;
        activeGenericTypeBindings = bindings;
        typeHelper.setGenericTypeBindings(activeGenericTypeBindings);

        IRBuilder<>::InsertPoint savedIP = builder.saveIP();

        VisitResult res = visitFunction(fit->second, &mangledName);

        if (savedIP.isSet()) {
            builder.restoreIP(savedIP);
        }

        activeGenericTypeBindings = oldBindings;
        typeHelper.setGenericTypeBindings(activeGenericTypeBindings);
        if (!res.value) return nullptr;
        return module->getFunction(mangledName);
    }
    
    bool ensureValidInsertPoint() {
        BasicBlock* currentBB = builder.GetInsertBlock();
        if (currentBB) return true;
        
        Function* mainFunc = module->getFunction("main");
        if (!mainFunc) {
            if (!mainFunctionCreated) {
                createDefaultMain();
                mainFunc = module->getFunction("main");
            } else {
                return false;
            }
        }
        
        if (mainFunc) {
            BasicBlock* lastBB = nullptr;
            for (auto& bb : *mainFunc) {
                lastBB = &bb;
            }
            
            if (lastBB && !lastBB->getTerminator()) {
                builder.SetInsertPoint(lastBB);
                return true;
            }
            
            BasicBlock* newBB = BasicBlock::Create(context, "block", mainFunc);
            builder.SetInsertPoint(newBB);
            return true;
        }
        
        return false;
    }
    
    Value* safeCreateGlobalStringPtr(const std::string& str, const std::string& name) {
        if (!ensureValidInsertPoint()) {
            return nullptr;
        }
        
        if (str.empty()) {
            return builder.CreateGlobalStringPtr("", name.c_str());
        }
        
        return builder.CreateGlobalStringPtr(str, name.c_str());
    }
    
    Type* getActualType(AllocaInst* alloc) {
        if (!alloc) return nullptr;
        return alloc->getAllocatedType();
    }

    Type* getLLVMTypeFromTypeInfo(const TypeInfo* info) {
        if (!info) return nullptr;

        switch (info->kind) {
            case TYPEINFO_VOID:
                return Type::getVoidTy(context);
            case TYPEINFO_I8:
                return Type::getInt8Ty(context);
            case TYPEINFO_I32:
                return Type::getInt32Ty(context);
            case TYPEINFO_I64:
                return Type::getInt64Ty(context);
            case TYPEINFO_F32:
                return Type::getFloatTy(context);
            case TYPEINFO_F64:
                return Type::getDoubleTy(context);
            case TYPEINFO_BOOL:
                return Type::getInt1Ty(context);
            case TYPEINFO_STRING:
                return PointerType::getUnqual(Type::getInt8Ty(context));
            case TYPEINFO_PTR: {
                Type* elem = getLLVMTypeFromTypeInfo(info->element);
                if (!elem) elem = Type::getInt8Ty(context);
                return PointerType::getUnqual(elem);
            }
            case TYPEINFO_STRUCT: {
                if (info->name) {
                    StructType* st = typeHelper.getStructType(info->name);
                    if (st) return st;
                }
                return PointerType::getUnqual(Type::getInt8Ty(context));
            }
            case TYPEINFO_ARRAY: {
                Type* elem = getLLVMTypeFromTypeInfo(info->element);
                if (!elem) elem = Type::getInt8Ty(context);
                return PointerType::getUnqual(elem);
            }
            case TYPEINFO_FIXED_ARRAY: {
                Type* elem = getLLVMTypeFromTypeInfo(info->element);
                if (!elem) elem = Type::getInt8Ty(context);
                return ArrayType::get(elem, info->size);
            }
            case TYPEINFO_FN:
                return PointerType::getUnqual(Type::getInt8Ty(context));
            case TYPEINFO_APP: {
                if (info->app_ctor && info->app_ctor->kind == TYPEINFO_STRUCT && info->app_ctor->name) {
                    StructType* st = typeHelper.getStructType(info->app_ctor->name);
                    if (st) return st;
                }
                return PointerType::getUnqual(Type::getInt8Ty(context));
            }
            case TYPEINFO_VAR:
            default:
                return PointerType::getUnqual(Type::getInt8Ty(context));
        }
    }

    ValueType getValueTypeFromTypeInfo(const TypeInfo* info) {
        if (!info) return ValueType::VOID;
        switch (info->kind) {
            case TYPEINFO_VOID: return ValueType::VOID;
            case TYPEINFO_I8: return ValueType::INT8;
            case TYPEINFO_I32: return ValueType::INT32;
            case TYPEINFO_I64: return ValueType::INT64;
            case TYPEINFO_F32: return ValueType::FLOAT32;
            case TYPEINFO_F64: return ValueType::FLOAT64;
            case TYPEINFO_BOOL: return ValueType::BOOL;
            case TYPEINFO_STRING: return ValueType::STRING;
            case TYPEINFO_ARRAY:
            case TYPEINFO_FIXED_ARRAY:
                return ValueType::ARRAY;
            case TYPEINFO_PTR:
            case TYPEINFO_STRUCT:
            case TYPEINFO_FN:
            case TYPEINFO_APP:
            case TYPEINFO_VAR:
            default:
                return ValueType::POINTER;
        }
    }

    Type* getInferredLLVMType(ASTNode* node) {
        if (!node || !node->inferred_type) return nullptr;
        return getLLVMTypeFromTypeInfo(node->inferred_type);
    }

    Type* getInferredArrayElementType(ASTNode* node) {
        if (!node || !node->inferred_type) return nullptr;
        const TypeInfo* info = node->inferred_type;
        if (info->kind == TYPEINFO_ARRAY || info->kind == TYPEINFO_FIXED_ARRAY) {
            return getLLVMTypeFromTypeInfo(info->element);
        }
        return nullptr;
    }

    Type* getInferredPointerElementType(ASTNode* node) {
        if (!node || !node->inferred_type) return nullptr;
        if (node->inferred_type->kind == TYPEINFO_PTR) {
            return getLLVMTypeFromTypeInfo(node->inferred_type->element);
        }
        return nullptr;
    }
    
    AllocaInst* findVariableInMain(const std::string& name) {
        Function* mainFunc = module->getFunction("main");
        if (!mainFunc) return nullptr;
        
        BasicBlock& entryBlock = mainFunc->getEntryBlock();
        for (Instruction& inst : entryBlock) {
            if (AllocaInst* alloc = dyn_cast<AllocaInst>(&inst)) {
                if (alloc->hasName() && alloc->getName() == name) {
                    return alloc;
                }
            }
        }
        return nullptr;
    }
    
    GlobalVariable* findGlobalVariable(const std::string& name) {
        return module->getGlobalVariable(name);
    }
    
    void initStrlen() {
        if (strlenFunction) return;
        
        FunctionType* strlenType = FunctionType::get(
            Type::getInt64Ty(context),
            {PointerType::getUnqual(Type::getInt8Ty(context))},
            false
        );
        
        strlenFunction = Function::Create(
            strlenType,
            Function::ExternalLinkage,
            "strlen",
            module.get()
        );
        strlenFunction->setCallingConv(CallingConv::C);
    }
    Type* getPointerElementTypeSafely(PointerType* ptrType, const std::string& varName) {
        if (!ptrType) return Type::getInt8Ty(context);

        if (varName.empty()) {
            return Type::getInt8Ty(context);
        }

        if (typeHelper.isStringVariable(varName)) {
            VIX_DEBUG_LOG << "[DEBUG] String variable '" << varName << "': using i8 as element type\n";
            return Type::getInt8Ty(context);
        }

        if (varName == "argv") {
            VIX_DEBUG_LOG << "[DEBUG] argv: using char** as element type\n";
            return PointerType::getUnqual(Type::getInt8Ty(context));
        }

        AllocaInst* alloc = scopeManager.findVariable(varName);
        if (!alloc) alloc = findVariableInMain(varName);
        if (alloc) {
            Type* allocatedType = getActualType(alloc);
            if (allocatedType && allocatedType->isArrayTy()) {
                ArrayType* arrType = cast<ArrayType>(allocatedType);
                Type* elemType = arrType->getElementType();
                VIX_DEBUG_LOG << "[DEBUG] Local array '" << varName
                             << "': using element type from alloc: " << *elemType << "\n";
                return elemType;
            }
        }

        if (auto* arrayInfo = typeHelper.getArrayTypeInfo(varName)) {
            VIX_DEBUG_LOG << "[DEBUG] Array variable '" << varName
                         << "': using element type from array info: " << *arrayInfo->first << "\n";
            return arrayInfo->first;
        }

        if (GlobalVariable* gv = module->getGlobalVariable(varName)) {
            Type* globalType = gv->getValueType();
            if (globalType->isArrayTy()) {
                ArrayType* arrType = cast<ArrayType>(globalType);
                Type* elemType = arrType->getElementType();
                VIX_DEBUG_LOG << "[DEBUG] Global array '" << varName
                             << "': using element type from global: " << *elemType << "\n";
                return elemType;
            }
        }

        if (varName.find("str") != std::string::npos ||
            varName.find("Str") != std::string::npos ||
            varName.find("STRING") != std::string::npos ||
            varName.find("lit") != std::string::npos) {
            VIX_DEBUG_LOG << "[DEBUG] Variable '" << varName
                         << "' looks like a string: using i8\n";
            return Type::getInt8Ty(context);
        }

        VIX_DEBUG_LOG << "[DEBUG] Unknown pointer '" << varName
                     << "': defaulting to i8\n";
        return Type::getInt8Ty(context);
    }

    bool isArrayParamPosition(const std::string& funcName, int userParamIndex) {
        auto it = functionArrayParamPositions.find(funcName);
        if (it == functionArrayParamPositions.end()) return false;
        for (int pos : it->second) {
            if (pos == userParamIndex) return true;
        }
        return false;
    }

    Value* getRuntimeArrayLengthValue(const std::string& varName) {
        std::string lenVarName = varName + "__len";
        AllocaInst* lenAlloc = scopeManager.findVariable(lenVarName);
        if (!lenAlloc) lenAlloc = findVariableInMain(lenVarName);
        if (!lenAlloc) return nullptr;

        Type* lenType = getActualType(lenAlloc);
        if (!lenType || !lenType->isIntegerTy()) return nullptr;

        Value* lenVal = builder.CreateLoad(lenType, lenAlloc, lenVarName + "_val");
        if (!lenVal->getType()->isIntegerTy(32)) {
            lenVal = builder.CreateIntCast(lenVal, Type::getInt32Ty(context), false, "arr_len_cast");
        }
        return lenVal;
    }

    Value* inferArrayLengthFromArgument(ASTNode* argNode) {
        if (argNode && argNode->type == AST_IDENTIFIER && argNode->data.identifier.name) {
            std::string varName(argNode->data.identifier.name);

            if (Value* runtimeLen = getRuntimeArrayLengthValue(varName)) {
                return runtimeLen;
            }

            AllocaInst* alloc = scopeManager.findVariable(varName);
            if (!alloc) alloc = findVariableInMain(varName);
            if (alloc) {
                Type* allocatedType = getActualType(alloc);
                if (allocatedType && allocatedType->isArrayTy()) {
                    uint64_t numElements = cast<ArrayType>(allocatedType)->getNumElements();
                    return ConstantInt::get(Type::getInt32Ty(context), numElements);
                }
            }

            if (auto* arrayInfo = typeHelper.getArrayTypeInfo(varName)) {
                if (arrayInfo->second >= 0) {
                    return ConstantInt::get(Type::getInt32Ty(context), arrayInfo->second);
                }
            }

            int knownSize = typeHelper.getVariableArraySize(varName);
            if (knownSize >= 0) {
                return ConstantInt::get(Type::getInt32Ty(context), knownSize);
            }
        }

        if (argNode && argNode->type == AST_EXPRESSION_LIST) {
            int count = argNode->data.expression_list.expression_count;
            return ConstantInt::get(Type::getInt32Ty(context), count);
        }

        return ConstantInt::get(Type::getInt32Ty(context), 0);
    }

    AllocaInst* findRuntimeArrayLengthSlot(const std::string& varName) {
        std::string lenVarName = varName + "__len";
        AllocaInst* lenAlloc = scopeManager.findVariable(lenVarName);
        if (!lenAlloc) lenAlloc = findVariableInMain(lenVarName);
        return lenAlloc;
    }

    AllocaInst* ensureRuntimeArrayLengthSlot(const std::string& varName, int initialLen) {
        AllocaInst* existing = findRuntimeArrayLengthSlot(varName);
        if (existing) return existing;

        Function* func = getCurrentFunction();
        if (!func) {
            func = module->getFunction("main");
            if (!func) {
                createDefaultMain();
                func = module->getFunction("main");
            }
        }
        if (!func) return nullptr;

        BasicBlock* entryBB = &func->getEntryBlock();
        BasicBlock* savedBB = builder.GetInsertBlock();

        std::string lenVarName = varName + "__len";
        IRBuilder<> tempBuilder(entryBB, entryBB->begin());
        AllocaInst* lenAlloc = tempBuilder.CreateAlloca(Type::getInt32Ty(context), nullptr, lenVarName);
        tempBuilder.CreateStore(ConstantInt::get(Type::getInt32Ty(context), initialLen), lenAlloc);

        if (savedBB) {
            builder.SetInsertPoint(savedBB);
        }

        scopeManager.defineVariable(lenVarName, lenAlloc);
        return lenAlloc;
    }

    Function* getOrCreateReallocFunction() {
        if (Function* fn = module->getFunction("realloc")) {
            return fn;
        }

        Type* i8PtrTy = PointerType::getUnqual(Type::getInt8Ty(context));
        Type* i64Ty = Type::getInt64Ty(context);
        FunctionType* reallocType = FunctionType::get(i8PtrTy, {i8PtrTy, i64Ty}, false);
        Function* reallocFn = Function::Create(reallocType, Function::ExternalLinkage, "realloc", module.get());
        reallocFn->setCallingConv(CallingConv::C);
        return reallocFn;
    }

    static constexpr uint64_t ARRAY_HEADER_BYTES = 8;

    Value* emitLoadArrayLength(Value* dataPtr, const Twine& name = "arr_len") {
        if (!dataPtr || !dataPtr->getType()->isPointerTy())
            return ConstantInt::get(Type::getInt32Ty(context), 0);

        PointerType* i8PtrTy = PointerType::getUnqual(Type::getInt8Ty(context));
        Value* dataI8 = builder.CreateBitCast(dataPtr, i8PtrTy, name + "_as_i8");

        Function* func = builder.GetInsertBlock()->getParent();
        BasicBlock* nullBB = BasicBlock::Create(context, name + "_null", func);
        BasicBlock* nonNullBB = BasicBlock::Create(context, name + "_nonnull", func);
        BasicBlock* mergeBB = BasicBlock::Create(context, name + "_merge", func);

        Value* isNull = builder.CreateIsNull(dataI8, name + "_is_null");
        BasicBlock* curBB = builder.GetInsertBlock();
        builder.CreateCondBr(isNull, nullBB, nonNullBB);

        builder.SetInsertPoint(nonNullBB);
        Value* ptrInt = builder.CreatePtrToInt(dataI8, Type::getInt64Ty(context));
        Value* headerInt = builder.CreateSub(ptrInt, ConstantInt::get(Type::getInt64Ty(context), ARRAY_HEADER_BYTES));
        Value* headerPtr = builder.CreateIntToPtr(headerInt, i8PtrTy);
        Value* lenPtr = builder.CreateBitCast(headerPtr, PointerType::getUnqual(Type::getInt32Ty(context)));
        Value* loadedLen = builder.CreateLoad(Type::getInt32Ty(context), lenPtr, name + "_loaded");
        builder.CreateBr(mergeBB);

        builder.SetInsertPoint(nullBB);
        builder.CreateBr(mergeBB);

        builder.SetInsertPoint(mergeBB);
        PHINode* result = builder.CreatePHI(Type::getInt32Ty(context), 2, name);
        result->addIncoming(ConstantInt::get(Type::getInt32Ty(context), 0), nullBB);
        result->addIncoming(loadedLen, nonNullBB);

        return result;
    }

    VisitResult emitFunctionPointerCall(Value* rawCalleePtr, ASTNode* argsNode, Type* expectedReturnTypeHint = nullptr) {
        if (!rawCalleePtr || !rawCalleePtr->getType()->isPointerTy()) return VisitResult();

        std::vector<Value*> argValues;
        std::vector<Type*> argTypes;
        if (argsNode && argsNode->type == AST_EXPRESSION_LIST) {
            int argCount = argsNode->data.expression_list.expression_count;
            for (int i = 0; i < argCount; i++) {
                ASTNode* argNode = argsNode->data.expression_list.expressions[i];
                VisitResult argRes = visit(argNode);
                if (!argRes.value) return VisitResult();
                argValues.push_back(argRes.value);
                argTypes.push_back(argRes.value->getType());
            }
        }

        Type* returnType = Type::getInt32Ty(context);
        if (expectedReturnTypeHint && !expectedReturnTypeHint->isVoidTy()) {
            returnType = expectedReturnTypeHint;
        } else if (!argTypes.empty()) {
            Type* firstArgType = argTypes[0];
            if (firstArgType->isIntegerTy() || firstArgType->isPointerTy() || firstArgType->isFloatingPointTy()) {
                returnType = firstArgType;
            }
        }

        FunctionType* fnType = FunctionType::get(returnType, argTypes, false);

        if ((int)fnType->getNumParams() != (int)argValues.size()) {
            return VisitResult();
        }

        for (size_t i = 0; i < argValues.size(); i++) {
            Type* expectedArgTy = fnType->getParamType((unsigned)i);
            Value* argVal = argValues[i];
            if (argVal->getType() != expectedArgTy) {
                ValueType toVT = typeHelper.getValueTypeFromType(expectedArgTy);
                ValueType fromVT = typeHelper.getValueTypeFromType(argVal->getType());
                argVal = typeHelper.castValue(builder, argVal, fromVT, toVT);
                if (argVal->getType() != expectedArgTy && argVal->getType()->isPointerTy() && expectedArgTy->isPointerTy()) {
                    argVal = builder.CreateBitCast(argVal, expectedArgTy, "fparg_ptrcast");
                }
                argValues[i] = argVal;
            }
        }

        Value* typedFnPtr = builder.CreateBitCast(rawCalleePtr, PointerType::getUnqual(fnType), "fnptr_cast");
        CallInst* callInst = builder.CreateCall(fnType, typedFnPtr, argValues, "fpcalltmp");
        return VisitResult(callInst, typeHelper.getValueTypeFromType(returnType));
    }

    Value* getBuiltinUnionCtorTagValue(const std::string& ctorName) {
        GlobalVariable* ctorGlobal = module->getGlobalVariable(ctorName, true);
        if (ctorGlobal && ctorGlobal->hasInitializer()) {
            if (auto* intInit = dyn_cast<ConstantInt>(ctorGlobal->getInitializer())) {
                return ConstantInt::get(Type::getInt32Ty(context), intInit->getSExtValue(), true);
            }
        }

        int32_t fallbackTag = 0;
        if (ctorName == "Some") fallbackTag = 1;
        if (ctorName == "Err") fallbackTag = 1;
        return ConstantInt::get(Type::getInt32Ty(context), fallbackTag, true);
    }

    Constant* evaluateConstExpr(ASTNode* node, ValueType* outType = nullptr) {
        if (!node) {
            if (outType) *outType = ValueType::INT32;
            return ConstantInt::get(Type::getInt32Ty(context), 0);
        }

        switch (node->type) {
            case AST_NUM_INT: {
                int64_t val = node->data.num_int.value;
                if (val >= -2147483648LL && val <= 2147483647LL) {
                    if (outType) *outType = ValueType::INT32;
                    return ConstantInt::get(Type::getInt32Ty(context), static_cast<int32_t>(val), true);
                }
                if (outType) *outType = ValueType::INT64;
                return ConstantInt::get(Type::getInt64Ty(context), static_cast<uint64_t>(val), true);//注意LLVM的ConstantInt::get对于64位整数需要传入uint64_t类型的值 即使是有符号的负数也要转换为对应的无符号表示
            }
            case AST_CHAR: {
                if (outType) *outType = ValueType::INT8;
                return ConstantInt::get(Type::getInt8Ty(context), static_cast<int8_t>(node->data.character.value), true);
            }
            case AST_IDENTIFIER: {
                if (!node->data.identifier.name) return nullptr;
                GlobalVariable* gv = module->getGlobalVariable(node->data.identifier.name, true);
                if (!gv) return nullptr;
                Constant* init = gv->getInitializer();
                if (!init) return nullptr;
                if (outType) *outType = typeHelper.getValueTypeFromType(init->getType());
                return init;
            }
            case AST_UNARYOP: {
                Constant* rhs = evaluateConstExpr(node->data.unaryop.expr, outType);
                if (!rhs) return nullptr;
                if (node->data.unaryop.op == OP_PLUS) return rhs;
                if (node->data.unaryop.op == OP_MINUS) {
                    if (auto* ci = dyn_cast<ConstantInt>(rhs)) {
                        int64_t value = ci->getSExtValue();
                        int bits = ci->getType()->isIntegerTy(64) ? 64 : 32;
                        if (outType) *outType = (bits == 64) ? ValueType::INT64 : ValueType::INT32;
                        if (bits == 64) {
                            return ConstantInt::get(Type::getInt64Ty(context), static_cast<uint64_t>(-value), true);
                        }
                        return ConstantInt::get(Type::getInt32Ty(context), static_cast<int32_t>(-value), true);
                    }
                    return nullptr;
                }
                return nullptr;
            }
            case AST_BINOP: {
                ValueType lt = ValueType::INT32;
                ValueType rt = ValueType::INT32;
                Constant* lhs = evaluateConstExpr(node->data.binop.left, &lt);
                Constant* rhs = evaluateConstExpr(node->data.binop.right, &rt);
                if (!lhs || !rhs) return nullptr;
                auto* li = dyn_cast<ConstantInt>(lhs);
                auto* ri = dyn_cast<ConstantInt>(rhs);
                if (!li || !ri) return nullptr;
                int64_t lv = li->getSExtValue();
                int64_t rv = ri->getSExtValue();
                int bits = (li->getType()->isIntegerTy(64) || ri->getType()->isIntegerTy(64)) ? 64 : 32;
                int64_t result = 0;
                switch (node->data.binop.op) {
                    case OP_ADD: result = lv + rv; break;
                    case OP_SUB: result = lv - rv; break;
                    case OP_MUL: result = lv * rv; break;
                    case OP_DIV: result = (rv == 0) ? 0 : (lv / rv); break;
                    case OP_MOD: result = (rv == 0) ? 0 : (lv % rv); break;
                    default: return nullptr;
                }
                if (bits == 64) {
                    if (outType) *outType = ValueType::INT64;
                    return ConstantInt::get(Type::getInt64Ty(context), static_cast<uint64_t>(result), true);
                }
                if (outType) *outType = ValueType::INT32;
                return ConstantInt::get(Type::getInt32Ty(context), static_cast<int32_t>(result), true);
            }
            case AST_STRUCT_LITERAL: {
                ASTNode* typeNameNode = node->data.struct_literal.type_name;
                if (!typeNameNode || typeNameNode->type != AST_IDENTIFIER || !typeNameNode->data.identifier.name) {
                    return nullptr;
                }

                std::string structName(typeNameNode->data.identifier.name);
                StructType* st = typeHelper.getStructType(structName);
                auto* fields = typeHelper.getStructFields(structName);
                if (!st || !fields) return nullptr;

                std::vector<Constant*> values(fields->size(), nullptr);
                for (size_t i = 0; i < fields->size(); ++i) {
                    values[i] = Constant::getNullValue((*fields)[i].second);
                }

                ASTNode* initFields = node->data.struct_literal.fields;
                if (initFields && initFields->type == AST_EXPRESSION_LIST) {
                    int count = initFields->data.expression_list.expression_count;
                    for (int i = 0; i < count; ++i) {
                        ASTNode* entry = initFields->data.expression_list.expressions[i];
                        if (!entry || entry->type != AST_ASSIGN || !entry->data.assign.left || !entry->data.assign.right) continue;
                        ASTNode* left = entry->data.assign.left;
                        if (left->type != AST_IDENTIFIER || !left->data.identifier.name) continue;

                        std::string fieldName(left->data.identifier.name);
                        int fieldIdx = typeHelper.getFieldIndex(structName, fieldName);
                        if (fieldIdx < 0 || static_cast<size_t>(fieldIdx) >= fields->size()) continue;

                        Constant* c = evaluateConstExpr(entry->data.assign.right, nullptr);
                        if (!c) continue;
                        Type* fty = (*fields)[fieldIdx].second;
                        if (c->getType() != fty) {
                            if (c->getType()->isIntegerTy() && fty->isIntegerTy()) {
                                int64_t v = cast<ConstantInt>(c)->getSExtValue();
                                c = ConstantInt::get(fty, static_cast<uint64_t>(v), true);
                            } else {
                                continue;
                            }
                        }
                        values[fieldIdx] = c;
                    }
                }

                if (outType) *outType = ValueType::POINTER;
                return ConstantStruct::get(st, values);
            }
            default:
                return nullptr;
        }
    }
    
public:
    struct VisitResult {
        Value* value;
        ValueType type;
        StructType* structType;
        
        VisitResult() : value(nullptr), type(ValueType::VOID), structType(nullptr) {}
        VisitResult(Value* v, ValueType t) : value(v), type(t), structType(nullptr) {}
        VisitResult(Value* v, ValueType t, StructType* st) : value(v), type(t), structType(st) {}
    };
    
    LLVMCodeGenerator() : builder(context), typeHelper(context) {
        module = std::make_unique<Module>("VixModule", context);
        std::string Triple = g_vix_target_triple.empty() ? sys::getProcessTriple() : g_vix_target_triple;
        llvm::Triple targetTriple(Triple);
        module->setTargetTriple(targetTriple.str());
        printfFunction = nullptr;
        strlenFunction = nullptr;
        isGlobalScope = true;
        mainFunctionCreated = false;
        sourceAttrs = parseSourceAttributes(current_input_filename);
        initTarget();
    }
    
    std::unique_ptr<Module> generate(ASTNode* ast_root) {
        if (!ast_root) return nullptr;
        
        initPrintf();
        initStrlen();
        visit(ast_root);
        
        bool hasMain = module->getFunction("main") != nullptr;
        
        if (!hasMain && !mainFunctionCreated && !sourceAttrs.noMain) {
            createDefaultMain();
        }
        Function* mainFunc = module->getFunction("main");
        if (mainFunc) {
            BasicBlock* endBB = nullptr;
            for (auto& BB : *mainFunc) {
                if (BB.getName() == "func_end") {
                    endBB = &BB;
                    break;
                }
            }
            if (!endBB) {
                endBB = BasicBlock::Create(context, "func_end", mainFunc);
            }
            std::vector<BasicBlock*> blocks;
            for (auto& BB : *mainFunc) blocks.push_back(&BB);
            for (BasicBlock* BB : blocks) {
                if (BB == endBB) continue;
                if (!BB->getTerminator()) {
                    IRBuilder<> tmpBuilder(BB);
                    tmpBuilder.CreateBr(endBB);
                }
            }
            if (!endBB->getTerminator()) {
                IRBuilder<> tmpBuilder(endBB);
                tmpBuilder.CreateRet(ConstantInt::get(Type::getInt32Ty(context), 0));
            }
        }
        std::string error;
        raw_string_ostream errorStream(error);
        if (verifyModule(*module, &errorStream)) {
            llvm::errs() << ";Module verification failed: " << error << "\n";
            module->print(llvm::errs(), nullptr);
        }
        
        return std::move(module);
    }
    
    void initPrintf() {
        if (printfFunction) return;
        std::vector<Type*> printfArgs;
        printfArgs.push_back(PointerType::getUnqual(Type::getInt8Ty(context)));
        FunctionType* printfType = FunctionType::get(
            Type::getInt32Ty(context), printfArgs, true);
        printfFunction = Function::Create(
            printfType, Function::ExternalLinkage, "printf", module.get());
        printfFunction->setCallingConv(CallingConv::C);
    }
    
    void createDefaultMain() {
        if (module->getFunction("main") || mainFunctionCreated) return;
        std::vector<Type*> mainParamTypes;
        mainParamTypes.push_back(Type::getInt32Ty(context));  // argc
        mainParamTypes.push_back(PointerType::getUnqual(PointerType::getUnqual(Type::getInt8Ty(context))));  // argv as char**

        FunctionType* mainType = FunctionType::get(
            Type::getInt32Ty(context), mainParamTypes, false);
        Function* mainFunc = Function::Create(
            mainType, Function::ExternalLinkage, "main", module.get());
        mainFunctionCreated = true;
        BasicBlock* entryBB = BasicBlock::Create(context, "entry", mainFunc);
        builder.SetInsertPoint(entryBB);
        auto arg_it = mainFunc->arg_begin();
        arg_it->setName("argc");
        (++arg_it)->setName("argv");

        scopeManager.setCurrentFunction(mainFunc);
    }
    
    Function* getCurrentFunction() {
        return scopeManager.getCurrentFunction();
    }
    
    VisitResult visit(ASTNode* node) {
        if (!node) return VisitResult();
        
        switch (node->type) {
            case AST_NUM_INT:      return visitNumInt(node);
            case AST_NUM_FLOAT:    return visitNumFloat(node);
            case AST_STRING:       return visitString(node);
            case AST_CHAR:         return visitChar(node);
            case AST_NIL:          return visitNil(node);
            case AST_IDENTIFIER:   return visitIdentifier(node);
            case AST_BINOP:        return visitBinOp(node);
            case AST_UNARYOP:      return visitUnaryOp(node);
            case AST_ASSIGN:       return visitAssign(node);
            case AST_PROGRAM:      return visitProgram(node);
            case AST_IF:           return visitIf(node);
            case AST_WHILE:        return visitWhile(node);
            case AST_FOR:          return visitFor(node);
            case AST_BREAK:        return visitBreak(node);
            case AST_CONTINUE:     return visitContinue(node);
            case AST_FUNCTION:     return visitFunction(node);
            case AST_CALL:         return visitCall(node);
            case AST_RETURN:       return visitReturn(node);
            case AST_PRINT:        return visitPrint(node);
            case AST_INPUT:        return visitInput(node);
            case AST_TOINT:        return visitToInt(node);
            case AST_TOFLOAT:      return visitToFloat(node);
            case AST_CONST:        return visitConst(node);
            case AST_STRUCT_DEF:   return visitStructDef(node);
            case AST_STRUCT_LITERAL: return visitStructLiteral(node);
            case AST_MEMBER_ACCESS: return visitMemberAccess(node);
            case AST_EXPRESSION_LIST: return visitArrayLiteral(node);
            case AST_INDEX:        return visitIndex(node);
            case AST_GLOBAL:       return visitGlobal(node);
            default:               return VisitResult();
        }
    }
    
    VisitResult visitNumInt(ASTNode* node) {
        int64_t val = node->data.num_int.value;
        ValueType type;
        Value* value;
        if (val >= -2147483648LL && val <= 2147483647LL) {
            type = ValueType::INT32;
            value = ConstantInt::get(Type::getInt32Ty(context), val, true);
        } else {
            type = ValueType::INT64;
            value = ConstantInt::get(Type::getInt64Ty(context), val, true);
        }
        return VisitResult(value, type);
    }
    
    VisitResult visitNumFloat(ASTNode* node) {
        double val = node->data.num_float.value;
        Value* value = ConstantFP::get(Type::getDoubleTy(context), val);
        return VisitResult(value, ValueType::FLOAT64);
    }
    
    VisitResult visitString(ASTNode* node) {
        if (!node) {
            Value* value = safeCreateGlobalStringPtr("", "empty_str_const");
            return VisitResult(value, ValueType::STRING);
        }
        
        const char* str = node->data.string.value;
        if (!str) {
            Value* value = safeCreateGlobalStringPtr("", "null_str_const");
            return VisitResult(value, ValueType::STRING);
        }
        
        Value* value = safeCreateGlobalStringPtr(str, "str_lit");
        return VisitResult(value, ValueType::STRING);
    }

    VisitResult visitChar(ASTNode* node) {
        if (!node) {
            Value* value = ConstantInt::get(Type::getInt8Ty(context), 0);
            return VisitResult(value, ValueType::INT8);
        }

        char ch = node->data.character.value;
        Value* value = ConstantInt::get(Type::getInt8Ty(context), static_cast<int8_t>(ch));
        return VisitResult(value, ValueType::INT8);
    }

    VisitResult visitNil(ASTNode* node) {
        (void)node;
        Type* nilPtrType = PointerType::getUnqual(Type::getInt8Ty(context));
        Value* nilValue = ConstantPointerNull::get(cast<PointerType>(nilPtrType));
        return VisitResult(nilValue, ValueType::POINTER);
    }

    VisitResult visitIdentifier(ASTNode* node) {
        if (!node || !node->data.identifier.name) return VisitResult();
        
        std::string name(node->data.identifier.name);
        if (name == "None") {
            Value* nil = ConstantPointerNull::get(PointerType::getUnqual(Type::getInt8Ty(context)));
            return VisitResult(nil, ValueType::POINTER);
        }
        if (name == "Some" || name == "Ok" || name == "Err") {
            return VisitResult(getBuiltinUnionCtorTagValue(name), ValueType::INT32);
        }
        if (isRegisteredUnionCtorName(name)) {
            GlobalVariable* gv = module->getGlobalVariable(name, true);
            if (gv && gv->hasInitializer()) {
                if (auto* intInit = dyn_cast<ConstantInt>(gv->getInitializer())) {
                    return VisitResult(ConstantInt::get(Type::getInt32Ty(context), intInit->getSExtValue(), true), ValueType::INT32);
                }
            }
            return VisitResult(ConstantInt::get(Type::getInt32Ty(context), ctorTagValue(name), true), ValueType::INT32);
        }
        AllocaInst* alloc = scopeManager.findVariable(name);
        Function* curFnForScope = getCurrentFunction();
        if (alloc && curFnForScope && alloc->getFunction() != curFnForScope) {
            reportCodegenSemanticError(node, "capturing local variables from outer functions is not supported yet");
            alloc = nullptr;
        }
        
        if (!alloc) {
            if (Function* fn = module->getFunction(name)) {
                return VisitResult(fn, ValueType::POINTER);
            }

            Function* curFn = getCurrentFunction();
            bool inMain = (curFn && curFn->getName() == "main");
            alloc = inMain ? findVariableInMain(name) : nullptr;
            if (alloc) {
                scopeManager.defineVariable(name, alloc);
            } else {
                if (!inMain && findVariableInMain(name)) {
                    reportCodegenSemanticError(node, "capturing local variables from outer functions is not supported yet");
                    return VisitResult(ConstantInt::get(Type::getInt32Ty(context), 0), ValueType::INT32);
                }
                GlobalVariable* globalVar = findGlobalVariable(name);
                if (globalVar) {
                    Type* globalType = globalVar->getValueType();
                    if (globalType && globalType->isStructTy()) {
                        StructType* st = cast<StructType>(globalType);
                        return VisitResult(globalVar, ValueType::POINTER, st);
                    }
                    Value* loadedValue = builder.CreateLoad(globalType, globalVar, name);
                    ValueType valueType = typeHelper.getValueTypeFromType(globalType);
                    return VisitResult(loadedValue, valueType);
                } else {
                    llvm::errs() << "Warning: Use of undeclared variable '" << name << "'\n";
                    return VisitResult(ConstantInt::get(Type::getInt32Ty(context), 0), ValueType::INT32);
                }
            }
        }
        
        Type* allocatedType = getActualType(alloc);
        
        // 检查是否是字符串变量
        bool isStringVar = typeHelper.isStringVariable(name);
        
        if (allocatedType && allocatedType->isStructTy()) {
            StructType* st = cast<StructType>(allocatedType);
            return VisitResult(alloc, ValueType::POINTER, st);
        }
        
        // 如果是字符串变量，需要正确处理
        if (isStringVar) {
            VIX_DEBUG_LOG << "[DEBUG] String variable '" << name << "' loading value\n";
            Value* val = builder.CreateLoad(allocatedType, alloc, name);
            // 对于字符串，返回的是指向字符数组或字符指针的值
            ValueType vt = typeHelper.getValueTypeFromType(allocatedType);
            return VisitResult(val, vt);
        }

        Value* val = builder.CreateLoad(allocatedType, alloc, name);
        ValueType type = typeHelper.getValueTypeFromType(allocatedType);
        if (node->inferred_type && node->inferred_type->kind == TYPEINFO_PTR) {
            Type* elemType = getLLVMTypeFromTypeInfo(node->inferred_type->element);
            if (elemType) {
                pointerElementHints[val] = elemType;
            }
        }
        return VisitResult(val, type);
    }
    
    // ==================== 修复：visitBinOp - 确保比较操作类型一致 ====================
    VisitResult visitBinOp(ASTNode* node) {
        if (!node || !node->data.binop.left || !node->data.binop.right)
            return VisitResult();
        
        VisitResult leftRes = visit(node->data.binop.left);
        VisitResult rightRes = visit(node->data.binop.right);
        if (!leftRes.value || !rightRes.value) {
            return VisitResult(ConstantInt::get(Type::getInt32Ty(context), 0), ValueType::INT32);
        }
        
        // 检查操作数类型并确保它们兼容
        // 特别处理 i8 类型的比较操作
        if ((leftRes.type == ValueType::INT8 || rightRes.type == ValueType::INT8) &&
            leftRes.value->getType()->isIntegerTy() && rightRes.value->getType()->isIntegerTy() &&
            (node->data.binop.op == OP_EQ || node->data.binop.op == OP_NE ||
             node->data.binop.op == OP_LT || node->data.binop.op == OP_LE ||
             node->data.binop.op == OP_GT || node->data.binop.op == OP_GE)) {
            
            // 对于比较操作，将两个操作数都提升到 i32 以确保类型一致
            Value* leftVal = leftRes.value;
            Value* rightVal = rightRes.value;
            
            // 确保两个操作数都是整数类型
            if (!leftVal->getType()->isIntegerTy() || !rightVal->getType()->isIntegerTy()) {
                llvm::errs() << "Error: Comparison operands must be integers\n";
                return VisitResult();
            }
            
            // 将两个操作数都扩展到 i32（如果它们不是 i32）
            if (leftVal->getType() != Type::getInt32Ty(context)) {
                if (leftVal->getType()->isIntegerTy(1)) {
                    leftVal = builder.CreateZExt(leftVal, Type::getInt32Ty(context), "left_zext");
                } else if (leftVal->getType()->getIntegerBitWidth() < 32) {
                    leftVal = builder.CreateSExt(leftVal, Type::getInt32Ty(context), "left_sext");
                }
            }
            
            if (rightVal->getType() != Type::getInt32Ty(context)) {
                if (rightVal->getType()->isIntegerTy(1)) {
                    rightVal = builder.CreateZExt(rightVal, Type::getInt32Ty(context), "right_zext");
                } else if (rightVal->getType()->getIntegerBitWidth() < 32) {
                    rightVal = builder.CreateSExt(rightVal, Type::getInt32Ty(context), "right_sext");
                }
            }
            
            // 执行比较操作
            switch (node->data.binop.op) {
                case OP_EQ:
                    return VisitResult(builder.CreateICmpEQ(leftVal, rightVal, "eqtmp"), ValueType::BOOL);
                case OP_NE:
                    return VisitResult(builder.CreateICmpNE(leftVal, rightVal, "netmp"), ValueType::BOOL);
                case OP_LT:
                    return VisitResult(builder.CreateICmpSLT(leftVal, rightVal, "lttmp"), ValueType::BOOL);
                case OP_LE:
                    return VisitResult(builder.CreateICmpSLE(leftVal, rightVal, "letmp"), ValueType::BOOL);
                case OP_GT:
                    return VisitResult(builder.CreateICmpSGT(leftVal, rightVal, "gttmp"), ValueType::BOOL);
                case OP_GE:
                    return VisitResult(builder.CreateICmpSGE(leftVal, rightVal, "getmp"), ValueType::BOOL);
                default:
                    return VisitResult();
            }
        }

        
        if (node->data.binop.op == OP_ADD || node->data.binop.op == OP_SUB) {//ptr +/- int, int + ptr
            bool leftIsPtr = leftRes.value->getType()->isPointerTy();
            bool rightIsPtr = rightRes.value->getType()->isPointerTy();
            bool leftIsInt = leftRes.value->getType()->isIntegerTy();
            bool rightIsInt = rightRes.value->getType()->isIntegerTy();
            if ((leftIsPtr && rightIsInt) ||
                (node->data.binop.op == OP_ADD && rightIsPtr && leftIsInt)) {
                Value* ptrVal = leftIsPtr ? leftRes.value : rightRes.value;
                Value* idxVal = leftIsPtr ? rightRes.value : leftRes.value;
                if (!idxVal->getType()->isIntegerTy(64)) {
                    idxVal = builder.CreateIntCast(idxVal, Type::getInt64Ty(context), true, "ptr_idx_cast");
                }
                if (node->data.binop.op == OP_SUB) {
                    idxVal = builder.CreateNeg(idxVal, "ptr_idx_neg");
                }
                std::string ptrName;
                ASTNode* ptrExpr = leftIsPtr ? node->data.binop.left : node->data.binop.right;
                if (ptrExpr && ptrExpr->type == AST_IDENTIFIER && ptrExpr->data.identifier.name) {
                    ptrName = std::string(ptrExpr->data.identifier.name);
                }
                Type* elemType = getPointerElementTypeSafely(
                    dyn_cast<PointerType>(ptrVal->getType()), ptrName);
                if (!elemType) {
                    elemType = Type::getInt32Ty(context);
                }
                Type* expectPtrType = PointerType::getUnqual(elemType);
                if (ptrVal->getType() != expectPtrType) {
                    ptrVal = builder.CreateBitCast(ptrVal, expectPtrType, "ptr_arith_cast");
                }
                Value* gep = builder.CreateInBoundsGEP(elemType, ptrVal, idxVal, "ptr_arith");
                return VisitResult(gep, ValueType::POINTER);
            }
        }

        bool isCompareOp = (node->data.binop.op == OP_EQ || node->data.binop.op == OP_NE ||
                            node->data.binop.op == OP_LT || node->data.binop.op == OP_LE ||
                            node->data.binop.op == OP_GT || node->data.binop.op == OP_GE);
        if (isCompareOp && (leftRes.value->getType()->isPointerTy() || rightRes.value->getType()->isPointerTy())) {
            Value* leftVal = leftRes.value;
            Value* rightVal = rightRes.value;
            Type* leftTy = leftVal->getType();
            Type* rightTy = rightVal->getType();

            if (leftTy->isPointerTy() && rightTy->isPointerTy()) {
                if (leftTy != rightTy) {
                    rightVal = builder.CreateBitCast(rightVal, leftTy, "cmp_ptr_cast");
                }
            } else if (leftTy->isPointerTy() && rightTy->isIntegerTy()) {
                if (auto* ci = dyn_cast<ConstantInt>(rightVal); ci && ci->isZero()) {
                    rightVal = ConstantPointerNull::get(cast<PointerType>(leftTy));
                } else {
                    if (!rightTy->isIntegerTy(64)) {
                        rightVal = builder.CreateIntCast(rightVal, Type::getInt64Ty(context), false, "cmp_int_cast64");
                    }
                    rightVal = builder.CreateIntToPtr(rightVal, cast<PointerType>(leftTy), "cmp_int_to_ptr");
                }
            } else if (rightTy->isPointerTy() && leftTy->isIntegerTy()) {
                if (auto* ci = dyn_cast<ConstantInt>(leftVal); ci && ci->isZero()) {
                    leftVal = ConstantPointerNull::get(cast<PointerType>(rightTy));
                } else {
                    if (!leftTy->isIntegerTy(64)) {
                        leftVal = builder.CreateIntCast(leftVal, Type::getInt64Ty(context), false, "cmp_int_cast64");
                    }
                    leftVal = builder.CreateIntToPtr(leftVal, cast<PointerType>(rightTy), "cmp_int_to_ptr");
                }
            }

            switch (node->data.binop.op) {
                case OP_EQ:
                    return VisitResult(builder.CreateICmpEQ(leftVal, rightVal, "eqtmp"), ValueType::BOOL);
                case OP_NE:
                    return VisitResult(builder.CreateICmpNE(leftVal, rightVal, "netmp"), ValueType::BOOL);
                case OP_LT:
                    return VisitResult(builder.CreateICmpULT(leftVal, rightVal, "lttmp"), ValueType::BOOL);
                case OP_LE:
                    return VisitResult(builder.CreateICmpULE(leftVal, rightVal, "letmp"), ValueType::BOOL);
                case OP_GT:
                    return VisitResult(builder.CreateICmpUGT(leftVal, rightVal, "gttmp"), ValueType::BOOL);
                case OP_GE:
                    return VisitResult(builder.CreateICmpUGE(leftVal, rightVal, "getmp"), ValueType::BOOL);
                default:
                    return VisitResult();
            }
        }

        auto [promotedLeftType, promotedRightType] = typeHelper.promoteTypes(
            leftRes.type, rightRes.type);
        
        Value* leftVal = typeHelper.castValue(builder, leftRes.value, 
                                              leftRes.type, promotedLeftType);
        Value* rightVal = typeHelper.castValue(builder, rightRes.value, 
                                               rightRes.type, promotedRightType);
        
        ValueType resultType = (promotedLeftType > promotedRightType) 
                               ? promotedLeftType : promotedRightType;
        bool isFloat = (resultType == ValueType::FLOAT32 || resultType == ValueType::FLOAT64);

        bool isArithmeticOp = (node->data.binop.op == OP_ADD || node->data.binop.op == OP_SUB ||
                               node->data.binop.op == OP_MUL || node->data.binop.op == OP_DIV ||
                               node->data.binop.op == OP_MOD);
        if (isArithmeticOp && !isFloat) {
            if (!leftVal->getType()->isIntegerTy() || !rightVal->getType()->isIntegerTy()) {
                reportCodegenSemanticError(node, "arithmetic operators require numeric operands");
                return VisitResult();
            }
        }

        auto toBoolValue = [&](Value* v, ValueType vt, const char* name) -> Value* {
            if (vt == ValueType::BOOL && v->getType()->isIntegerTy(1)) {
                return v;
            }
            Type* ty = v->getType();
            if (ty->isIntegerTy()) {
                return builder.CreateICmpNE(v, ConstantInt::get(ty, 0), name);
            }
            if (ty->isFloatingPointTy()) {
                return builder.CreateFCmpONE(v, ConstantFP::get(ty, 0.0), name);
            }
            if (ty->isPointerTy()) {
                return builder.CreateICmpNE(v, ConstantPointerNull::get(cast<PointerType>(ty)), name);
            }
            return ConstantInt::getFalse(context);
        };
        
        switch (node->data.binop.op) {
            case OP_ADD:
                if (isFloat) return VisitResult(builder.CreateFAdd(leftVal, rightVal, "addtmp"), resultType);
                else return VisitResult(builder.CreateAdd(leftVal, rightVal, "addtmp"), resultType);
            case OP_SUB:
                if (isFloat) return VisitResult(builder.CreateFSub(leftVal, rightVal, "subtmp"), resultType);
                else return VisitResult(builder.CreateSub(leftVal, rightVal, "subtmp"), resultType);
            case OP_MUL:
                if (isFloat) return VisitResult(builder.CreateFMul(leftVal, rightVal, "multmp"), resultType);
                else return VisitResult(builder.CreateMul(leftVal, rightVal, "multmp"), resultType);
            case OP_DIV:
                if (isFloat) return VisitResult(builder.CreateFDiv(leftVal, rightVal, "divtmp"), resultType);
                else return VisitResult(builder.CreateSDiv(leftVal, rightVal, "divtmp"), resultType);
            case OP_MOD:
                if (isFloat) return VisitResult();
                else return VisitResult(builder.CreateSRem(leftVal, rightVal, "modtmp"), resultType);
            case OP_EQ:
                if (isFloat) return VisitResult(builder.CreateFCmpOEQ(leftVal, rightVal, "eqtmp"), ValueType::BOOL);
                else return VisitResult(builder.CreateICmpEQ(leftVal, rightVal, "eqtmp"), ValueType::BOOL);
            case OP_NE:
                if (isFloat) return VisitResult(builder.CreateFCmpONE(leftVal, rightVal, "netmp"), ValueType::BOOL);
                else return VisitResult(builder.CreateICmpNE(leftVal, rightVal, "netmp"), ValueType::BOOL);
            case OP_LT:
                if (isFloat) return VisitResult(builder.CreateFCmpOLT(leftVal, rightVal, "lttmp"), ValueType::BOOL);
                else return VisitResult(builder.CreateICmpSLT(leftVal, rightVal, "lttmp"), ValueType::BOOL);
            case OP_LE:
                if (isFloat) return VisitResult(builder.CreateFCmpOLE(leftVal, rightVal, "letmp"), ValueType::BOOL);
                else return VisitResult(builder.CreateICmpSLE(leftVal, rightVal, "letmp"), ValueType::BOOL);
            case OP_GT:
                if (isFloat) return VisitResult(builder.CreateFCmpOGT(leftVal, rightVal, "gttmp"), ValueType::BOOL);
                else return VisitResult(builder.CreateICmpSGT(leftVal, rightVal, "gttmp"), ValueType::BOOL);
            case OP_GE:
                if (isFloat) return VisitResult(builder.CreateFCmpOGE(leftVal, rightVal, "getmp"), ValueType::BOOL);
                else return VisitResult(builder.CreateICmpSGE(leftVal, rightVal, "getmp"), ValueType::BOOL);
            case OP_AND:
                {
                    Value* leftBool = toBoolValue(leftRes.value, leftRes.type, "and_lhs");
                    Value* rightBool = toBoolValue(rightRes.value, rightRes.type, "and_rhs");
                    Value* result = builder.CreateAnd(leftBool, rightBool, "andtmp");
                    return VisitResult(result, ValueType::BOOL);
                }
            case OP_OR:
                {
                    Value* leftBool = toBoolValue(leftRes.value, leftRes.type, "or_lhs");
                    Value* rightBool = toBoolValue(rightRes.value, rightRes.type, "or_rhs");
                    Value* result = builder.CreateOr(leftBool, rightBool, "ortmp");
                    return VisitResult(result, ValueType::BOOL);
                }
            default: return VisitResult();
        }
    }
    
    VisitResult visitUnaryOp(ASTNode* node) {
        if (!node || !node->data.unaryop.expr) return VisitResult();

        if (node->data.unaryop.op == OP_ADDRESS) {
            ASTNode* expr = node->data.unaryop.expr;
            if (expr->type == AST_STRUCT_LITERAL) {
                VisitResult structRes = visit(expr);
                if (structRes.value) {
                    return structRes;
                }
                return VisitResult();
            }
            if (expr->type == AST_IDENTIFIER && expr->data.identifier.name) {
                std::string name(expr->data.identifier.name);
                AllocaInst* alloc = scopeManager.findVariable(name);
                if (!alloc) alloc = findVariableInMain(name);
                if (alloc) {
                    return VisitResult(alloc, ValueType::POINTER);
                }

                GlobalVariable* gvar = findGlobalVariable(name);
                if (gvar) {
                    return VisitResult(gvar, ValueType::POINTER);
                }

                std::cerr << "Warning: Address-of operator applied to undefined variable '"
                          << name << "'" << std::endl;
            }

            if (expr->type == AST_MEMBER_ACCESS) {
                ASTNode* object = expr->data.member_access.object;
                ASTNode* field = expr->data.member_access.field;
                if (!object || !field || field->type != AST_IDENTIFIER) {
                    return VisitResult();
                }

                std::string fieldName(field->data.identifier.name);
                VisitResult objectRes = visit(object);
                if (!objectRes.value) {
                    return VisitResult();
                }

                StructType* structType = nullptr;
                Value* basePtr = nullptr;

                if (objectRes.structType) {
                    structType = objectRes.structType;
                    basePtr = objectRes.value;
                }

                if (!structType && object->type == AST_IDENTIFIER && object->data.identifier.name) {
                    std::string objName(object->data.identifier.name);
                    AllocaInst* alloc = scopeManager.findVariable(objName);
                    if (!alloc) alloc = findVariableInMain(objName);
                    if (alloc) {
                        Type* allocType = getActualType(alloc);
                        if (allocType && allocType->isStructTy()) {
                            structType = cast<StructType>(allocType);
                            basePtr = alloc;
                        }
                    }
                }

                if (!structType || !basePtr) {
                    return VisitResult();
                }

                std::string structName = structType->getName().str();
                int idx = typeHelper.getFieldIndex(structName, fieldName);
                if (idx < 0) {
                    return VisitResult();
                }

                Value* fieldPtr = builder.CreateStructGEP(structType, basePtr, idx, fieldName + "_addr");
                Type* fieldType = structType->getElementType(idx);
                if (fieldType->isStructTy()) {
                    return VisitResult(fieldPtr, ValueType::POINTER, cast<StructType>(fieldType));
                }
                return VisitResult(fieldPtr, ValueType::POINTER);
            }

            if (expr->type == AST_INDEX && expr->data.index.target && expr->data.index.index) {
                ASTNode* target = expr->data.index.target;
                ASTNode* indexExpr = expr->data.index.index;

                VisitResult idxRes = visit(indexExpr);
                if (!idxRes.value) return VisitResult();
                Value* idxVal = idxRes.value;
                if (!idxVal->getType()->isIntegerTy(32)) {
                    idxVal = builder.CreateIntCast(idxVal, Type::getInt32Ty(context), true, "addr_idx_cast");
                }

                AllocaInst* baseAlloc = nullptr;
                std::string varName;
                if (target->type == AST_IDENTIFIER && target->data.identifier.name) {
                    varName = std::string(target->data.identifier.name);
                    baseAlloc = scopeManager.findVariable(varName);
                    if (!baseAlloc) baseAlloc = findVariableInMain(varName);
                }

                if (baseAlloc) {
                    Type* allocatedType = getActualType(baseAlloc);
                    if (allocatedType && allocatedType->isArrayTy()) {
                        Value* gep = builder.CreateInBoundsGEP(
                            allocatedType,
                            baseAlloc,
                            {ConstantInt::get(Type::getInt32Ty(context), 0), idxVal},
                            "addr_arr_index_ptr");
                        return VisitResult(gep, ValueType::POINTER);
                    }

                    if (allocatedType && allocatedType->isPointerTy()) {
                        Value* arrayPtr = builder.CreateLoad(allocatedType, baseAlloc, "addr_array_ptr");
                        Type* elemType = getPointerElementTypeSafely(
                            dyn_cast<PointerType>(allocatedType), varName);
                        Value* gep = builder.CreateInBoundsGEP(elemType, arrayPtr, idxVal, "addr_ptr_index_ptr");
                        return VisitResult(gep, ValueType::POINTER);
                    }
                }

                VisitResult targetRes = visit(target);
                if (!targetRes.value) return VisitResult();
                if (targetRes.value->getType()->isPointerTy()) {
                    Type* elemType = getPointerElementTypeSafely(
                        dyn_cast<PointerType>(targetRes.value->getType()), varName);
                    Value* gep = builder.CreateInBoundsGEP(elemType, targetRes.value, idxVal, "addr_idx_ptr");
                    return VisitResult(gep, ValueType::POINTER);
                }
            }
            return VisitResult();//no valid addr target
        }

        VisitResult operand = visit(node->data.unaryop.expr);
        if (!operand.value) return VisitResult();

        switch (node->data.unaryop.op) {
            case OP_MINUS:
                if (operand.type == ValueType::FLOAT32 || operand.type == ValueType::FLOAT64)
                    return VisitResult(builder.CreateFNeg(operand.value, "negtmp"), operand.type);
                else
                    return VisitResult(builder.CreateNeg(operand.value, "negtmp"), operand.type);
            case OP_PLUS:
                return operand;
            case OP_DEREF: {
                Value* ptrVal = operand.value;
                if (!ptrVal->getType()->isPointerTy()) {
                    return VisitResult();
                }

                std::string varName;
                ASTNode* derefExpr = node->data.unaryop.expr;
                if (derefExpr &&
                    derefExpr->type == AST_IDENTIFIER &&
                    derefExpr->data.identifier.name) {
                    varName = std::string(derefExpr->data.identifier.name);
                } else if (derefExpr && derefExpr->type == AST_BINOP &&
                           (derefExpr->data.binop.op == OP_ADD || derefExpr->data.binop.op == OP_SUB)) {
                    ASTNode* left = derefExpr->data.binop.left;
                    ASTNode* right = derefExpr->data.binop.right;
                    if (left && left->type == AST_IDENTIFIER && left->data.identifier.name) {
                        varName = std::string(left->data.identifier.name);
                    } else if (right && right->type == AST_IDENTIFIER && right->data.identifier.name) {
                        varName = std::string(right->data.identifier.name);
                    }
                }

                PointerType* ptrTy = dyn_cast<PointerType>(ptrVal->getType());
                Type* elemType = getPointerElementTypeSafely(ptrTy, varName);
                if (!elemType) {
                    elemType = Type::getInt32Ty(context);
                }

                Type* expectPtrType = PointerType::getUnqual(elemType);
                if (ptrVal->getType() != expectPtrType) {
                    ptrVal = builder.CreateBitCast(ptrVal, expectPtrType, "deref_ptrcast");
                }

                Value* loaded = builder.CreateLoad(elemType, ptrVal, "deref_load");
                return VisitResult(loaded, typeHelper.getValueTypeFromType(elemType));
            }
            default:
                return VisitResult();
        }
    }
    
    VisitResult visitAssign(ASTNode* node) {
        if (!node || !node->data.assign.left || !node->data.assign.right)
            return VisitResult();
        
        if (node->data.assign.right->type == AST_STRUCT_LITERAL) {
            return visitStructAssign(node);
        }
        
        if (node->data.assign.left->type == AST_MEMBER_ACCESS) {
            return visitMemberAssign(node);
        }

        if (node->data.assign.left->type == AST_INDEX) {
            return visitIndexAssign(node);
        }

        if (node->data.assign.left->type == AST_UNARYOP &&
            node->data.assign.left->data.unaryop.op == OP_DEREF) {
            ASTNode* ptrExpr = node->data.assign.left->data.unaryop.expr;
            VisitResult ptrRes = visit(ptrExpr);
            if (!ptrRes.value || !ptrRes.value->getType()->isPointerTy()) {
                return VisitResult();
            }

            std::string ptrVarName;
            if (ptrExpr && ptrExpr->type == AST_IDENTIFIER && ptrExpr->data.identifier.name) {
                ptrVarName = std::string(ptrExpr->data.identifier.name);
            } else if (ptrExpr && ptrExpr->type == AST_BINOP &&
                       (ptrExpr->data.binop.op == OP_ADD || ptrExpr->data.binop.op == OP_SUB)) {
                ASTNode* left = ptrExpr->data.binop.left;
                ASTNode* right = ptrExpr->data.binop.right;
                if (left && left->type == AST_IDENTIFIER && left->data.identifier.name) {
                    ptrVarName = std::string(left->data.identifier.name);
                } else if (right && right->type == AST_IDENTIFIER && right->data.identifier.name) {
                    ptrVarName = std::string(right->data.identifier.name);
                }
            }

            Type* elemType = getPointerElementTypeSafely(
                dyn_cast<PointerType>(ptrRes.value->getType()), ptrVarName);
            if (!elemType) {
                elemType = Type::getInt32Ty(context);
            }

            Value* ptrVal = ptrRes.value;
            Type* expectPtrType = PointerType::getUnqual(elemType);
            if (ptrVal->getType() != expectPtrType) {
                ptrVal = builder.CreateBitCast(ptrVal, expectPtrType, "deref_store_ptrcast");
            }//处理赋值右侧的值

            VisitResult rightVal = visit(node->data.assign.right);
            if (!rightVal.value) return VisitResult();

            ValueType targetType = typeHelper.getValueTypeFromType(elemType);
            Value* casted = typeHelper.castValue(builder, rightVal.value, rightVal.type, targetType);

            if (casted->getType() != elemType) {
                if (casted->getType()->isPointerTy() && elemType->isPointerTy()) {
                    casted = builder.CreateBitCast(casted, elemType, "deref_rhs_ptrcast");
                } else if (casted->getType()->isIntegerTy() && elemType->isIntegerTy()) {
                    casted = builder.CreateIntCast(casted, elemType, true, "deref_rhs_intcast");
                }
            }

            builder.CreateStore(casted, ptrVal);
            return VisitResult(casted, targetType);
        }//处理普通变量赋值

        if (node->data.assign.left->type != AST_IDENTIFIER)
            return VisitResult();
        
        std::string name(node->data.assign.left->data.identifier.name);
        VisitResult rightVal = visit(node->data.assign.right);
        if (!rightVal.value) return VisitResult();

        if (rightVal.structType && rightVal.value->getType()->isPointerTy()) {
            AllocaInst* structAlloc = scopeManager.findVariable(name);
            if (!structAlloc) {
                structAlloc = findVariableInMain(name);
                if (structAlloc) {
                    scopeManager.defineVariable(name, structAlloc);
                }
            }

            if (!structAlloc) {
                Function* func = getCurrentFunction();
                if (!func) {
                    func = module->getFunction("main");
                    if (!func) {
                        createDefaultMain();
                        func = module->getFunction("main");
                    }
                }
                if (!func) return VisitResult();

                BasicBlock* entryBB = &func->getEntryBlock();
                BasicBlock* savedBB = builder.GetInsertBlock();
                IRBuilder<> tempBuilder(entryBB, entryBB->begin());
                structAlloc = tempBuilder.CreateAlloca(rightVal.structType, nullptr, name);
                if (savedBB) {
                    builder.SetInsertPoint(savedBB);
                }
                scopeManager.defineVariable(name, structAlloc);
            }

            Type* dstType = getActualType(structAlloc);
            if (!dstType || dstType != rightVal.structType) {
                return VisitResult();
            }

            Value* srcPtr = rightVal.value;
            Type* expectPtrType = PointerType::getUnqual(rightVal.structType);
            if (srcPtr->getType() != expectPtrType) {
                srcPtr = builder.CreateBitCast(srcPtr, expectPtrType, "ret_struct_ptrcast");
            }
            Value* srcValue = builder.CreateLoad(rightVal.structType, srcPtr, name + "_ret_struct_val");
            builder.CreateStore(srcValue, structAlloc);
            return VisitResult(structAlloc, ValueType::POINTER, rightVal.structType);
        }

        Type* inferredPointerElementType = nullptr;
        if (node->data.assign.right->type == AST_UNARYOP &&
            node->data.assign.right->data.unaryop.op == OP_ADDRESS) {
            ASTNode* addrExpr = node->data.assign.right->data.unaryop.expr;
            if (addrExpr && addrExpr->type == AST_IDENTIFIER && addrExpr->data.identifier.name) {
                std::string baseName(addrExpr->data.identifier.name);
                AllocaInst* baseAlloc = scopeManager.findVariable(baseName);
                if (!baseAlloc) baseAlloc = findVariableInMain(baseName);
                if (baseAlloc) {
                    inferredPointerElementType = getActualType(baseAlloc);
                } else if (GlobalVariable* baseGlobal = findGlobalVariable(baseName)) {
                    inferredPointerElementType = baseGlobal->getValueType();
                }
            }

            if (!inferredPointerElementType &&
                addrExpr && addrExpr->type == AST_INDEX &&
                addrExpr->data.index.target &&
                addrExpr->data.index.target->type == AST_IDENTIFIER &&
                addrExpr->data.index.target->data.identifier.name) {
                std::string baseName(addrExpr->data.index.target->data.identifier.name);
                if (auto* info = typeHelper.getArrayTypeInfo(baseName)) {
                    inferredPointerElementType = info->first;
                } else {
                    AllocaInst* baseAlloc = scopeManager.findVariable(baseName);
                    if (!baseAlloc) baseAlloc = findVariableInMain(baseName);
                    if (baseAlloc) {
                        Type* baseType = getActualType(baseAlloc);
                        if (baseType && baseType->isArrayTy()) {
                            inferredPointerElementType = cast<ArrayType>(baseType)->getElementType();
                        }
                    }
                }
            }

            if (!inferredPointerElementType &&
                addrExpr && addrExpr->type == AST_MEMBER_ACCESS) {
                ASTNode* object = addrExpr->data.member_access.object;
                ASTNode* field = addrExpr->data.member_access.field;
                if (object && field && field->type == AST_IDENTIFIER && field->data.identifier.name) {
                    StructType* structType = nullptr;

                    if (object->type == AST_IDENTIFIER && object->data.identifier.name) {
                        std::string objName(object->data.identifier.name);
                        AllocaInst* baseAlloc = scopeManager.findVariable(objName);
                        if (!baseAlloc) baseAlloc = findVariableInMain(objName);
                        if (baseAlloc) {
                            Type* baseType = getActualType(baseAlloc);
                            if (baseType && baseType->isStructTy()) {
                                structType = cast<StructType>(baseType);
                            }
                        }
                    }

                    if (structType) {
                        std::string structName = structType->getName().str();
                        int idx = typeHelper.getFieldIndex(structName, std::string(field->data.identifier.name));
                        if (idx >= 0) {
                            inferredPointerElementType = structType->getElementType(idx);
                        }
                    }
                }
            }
        }//如果赋值右侧是字符串字面量 或者是字符串变量 也可以尝试推断元素类型为i8
        bool isStringAssign = (node->data.assign.right->type == AST_STRING);
        
        AllocaInst* alloc = scopeManager.findVariable(name);
        if (!alloc) {
            GlobalVariable* gvar = findGlobalVariable(name);
            if (gvar) {
                Function* func = getCurrentFunction();
                if (!func) {
                    func = module->getFunction("main");
                    if (!func) {
                        createDefaultMain();
                        func = module->getFunction("main");
                    }
                }

                Type* globalType = gvar->getValueType();
                ValueType gvt = typeHelper.getValueTypeFromType(globalType);
                Value* castedVal = typeHelper.castValue(builder, rightVal.value, rightVal.type, gvt);
                builder.CreateStore(castedVal, gvar);
                return VisitResult(castedVal, gvt);
            }
            
            Function* func = getCurrentFunction();
            if (!func) {
                func = module->getFunction("main");
                if (!func) {
                    createDefaultMain();
                    func = module->getFunction("main");
                }
            }
            if (!func) return VisitResult();
            
            BasicBlock* entryBB = &func->getEntryBlock();
            BasicBlock* savedBB = builder.GetInsertBlock();
            
            Type* varType = nullptr;
            if (isStringAssign) {
                varType = PointerType::getUnqual(Type::getInt8Ty(context));
            } else {
                varType = rightVal.value->getType();
            }
            
            IRBuilder<> tempBuilder(entryBB, entryBB->begin());
            alloc = tempBuilder.CreateAlloca(varType, nullptr, name);
            
            if (savedBB) {
                builder.SetInsertPoint(savedBB);
            }
            scopeManager.defineVariable(name, alloc);
            
            if (node->data.assign.right->type == AST_STRING) {
                const char* s_val = node->data.assign.right->data.string.value;
                int strlen_val = 0;
                if (s_val) strlen_val = (int)strlen(s_val);
                typeHelper.registerArrayType(name, Type::getInt8Ty(context), strlen_val);
                typeHelper.registerVariableArraySize(name, strlen_val);
                typeHelper.registerStringVariable(name);
            }

            if (node->data.assign.right->type == AST_EXPRESSION_LIST) {
                int arraySize = node->data.assign.right->data.expression_list.expression_count;
                typeHelper.registerVariableArraySize(name, arraySize);
                
                if (rightVal.value && rightVal.value->getType()->isPointerTy()) {
                    Type* elemType = getInferredArrayElementType(node->data.assign.right);
                    bool hasInferredElem = (elemType != nullptr);
                    if (!elemType) {
                        elemType = Type::getInt32Ty(context);
                    }
                    if (!hasInferredElem && arraySize > 0) {
                        ASTNode* firstElem = node->data.assign.right->data.expression_list.expressions[0];
                        if (firstElem) {
                            if (firstElem->type == AST_STRING) {
                                elemType = PointerType::getUnqual(Type::getInt8Ty(context));
                            } else if (firstElem->type == AST_CHAR) {
                                elemType = Type::getInt8Ty(context);
                            } else if (firstElem->type == AST_NUM_FLOAT) {
                                elemType = Type::getDoubleTy(context);
                            } else if (firstElem->type == AST_NUM_INT) {
                                int64_t v = firstElem->data.num_int.value;
                                elemType = (v >= -2147483648LL && v <= 2147483647LL)
                                           ? Type::getInt32Ty(context)
                                           : Type::getInt64Ty(context);
                            }
                        }
                    }
                    typeHelper.registerArrayType(name, elemType, arraySize);
                }
            }

            auto hintIt = pointerElementHints.find(rightVal.value);
            if (hintIt != pointerElementHints.end() && hintIt->second &&
                node->data.assign.right->type != AST_EXPRESSION_LIST) {
                typeHelper.registerArrayType(name, hintIt->second, -1);
                typeHelper.registerVariableArraySize(name, -1);
            }

            if (inferredPointerElementType) {
                typeHelper.registerArrayType(name, inferredPointerElementType, -1);
            }//如果是字符串赋值 也注册为字符串变量
        }
        
        Type* allocatedType = getActualType(alloc);
        ValueType varType = typeHelper.getValueTypeFromType(allocatedType);
        Value* val = typeHelper.castValue(builder, rightVal.value, rightVal.type, varType);
        if (inferredPointerElementType) {
            typeHelper.registerArrayType(name, inferredPointerElementType, -1);
        }
        builder.CreateStore(val, alloc);
        return VisitResult(val, varType);
    }
    
    VisitResult visitIndexAssign(ASTNode* node) {
        ASTNode* indexNode = node->data.assign.left;
        ASTNode* target = indexNode->data.index.target;
        ASTNode* idxExpr = indexNode->data.index.index;

        VisitResult idxRes = visit(idxExpr);
        if (!idxRes.value) return VisitResult();
        Value* idxVal = idxRes.value;
        if (!idxVal->getType()->isIntegerTy(32)) {
            idxVal = builder.CreateIntCast(idxVal, Type::getInt32Ty(context), true, "idxcast");
        }

        AllocaInst* baseAlloc = nullptr;
        std::string varName;
        if (target->type == AST_IDENTIFIER) {
            varName = std::string(target->data.identifier.name);
            baseAlloc = scopeManager.findVariable(varName);
            if (!baseAlloc) baseAlloc = findVariableInMain(varName);
        }

        if (baseAlloc) {
            Type* allocatedType = getActualType(baseAlloc);
            if (allocatedType && allocatedType->isArrayTy()) {
                ArrayType* at = cast<ArrayType>(allocatedType);
                Type* elemType = at->getElementType();
                Value* gep = builder.CreateInBoundsGEP(allocatedType, baseAlloc, 
                    {ConstantInt::get(Type::getInt32Ty(context),0), idxVal}, "arr_index_ptr");

                VisitResult rightVal = visit(node->data.assign.right);
                if (!rightVal.value) return VisitResult();
                ValueType vt = typeHelper.getValueTypeFromType(elemType);
                Value* casted = typeHelper.castValue(builder, rightVal.value, rightVal.type, vt);
                builder.CreateStore(casted, gep);
                return VisitResult(casted, vt);
            }
            
            if (allocatedType && allocatedType->isPointerTy()) {
                Value* arrayPtr = builder.CreateLoad(allocatedType, baseAlloc, "array_ptr");
                Type* elemType = getPointerElementTypeSafely(dyn_cast<PointerType>(allocatedType), varName);
                if (varName == "argv") {
                    elemType = PointerType::getUnqual(Type::getInt8Ty(context));
                }

                Value* gep = builder.CreateInBoundsGEP(elemType, arrayPtr, idxVal, "ptr_index_ptr");
                VisitResult rightVal = visit(node->data.assign.right);
                if (!rightVal.value) return VisitResult();
                if (!varName.empty() && varName != "argv" && !typeHelper.getArrayTypeInfo(varName)) {
                    typeHelper.registerArrayType(varName, rightVal.value->getType(), -1);
                    elemType = rightVal.value->getType();
                    gep = builder.CreateInBoundsGEP(elemType, arrayPtr, idxVal, "ptr_index_ptr");
                }
                ValueType vt = typeHelper.getValueTypeFromType(elemType);
                Value* casted = typeHelper.castValue(builder, rightVal.value, rightVal.type, vt);
                builder.CreateStore(casted, gep);
                return VisitResult(casted, vt);
            }

            if (allocatedType && allocatedType->isIntegerTy()) {
                Value* baseInt = builder.CreateLoad(allocatedType, baseAlloc, "addr_base");
                if (!baseInt->getType()->isIntegerTy(64)) {
                    baseInt = builder.CreateSExtOrTrunc(baseInt, Type::getInt64Ty(context), "addr_base64");
                }
                Value* idx64 = idxVal;
                if (!idx64->getType()->isIntegerTy(64)) {
                    idx64 = builder.CreateSExtOrTrunc(idx64, Type::getInt64Ty(context), "idx64");
                }

                VisitResult rightVal = visit(node->data.assign.right);
                if (!rightVal.value) return VisitResult();

                Type* elemType = Type::getInt8Ty(context);
                ValueType vt = ValueType::INT8;
                Value* basePtr = builder.CreateIntToPtr(baseInt, PointerType::getUnqual(elemType), "mmio_ptr");
                Value* gep = builder.CreateInBoundsGEP(elemType, basePtr, idx64, "mmio_index_ptr");
                Value* casted = typeHelper.castValue(builder, rightVal.value, rightVal.type, vt);
                if (!casted->getType()->isIntegerTy(8)) {
                    casted = builder.CreateIntCast(casted, Type::getInt8Ty(context), true, "mmio_i8");
                }
                builder.CreateStore(casted, gep);
                return VisitResult(casted, vt);
            }
        }

        VisitResult targRes = visit(target);
        if (!targRes.value) return VisitResult();

        if (targRes.value->getType()->isPointerTy()) {
            if (target->type == AST_IDENTIFIER) {
                varName = std::string(target->data.identifier.name);
            }
            Type* elemType = getPointerElementTypeSafely(dyn_cast<PointerType>(targRes.value->getType()), varName);

            Value* gep = builder.CreateInBoundsGEP(elemType, targRes.value, idxVal, "arr_index_ptr");

            if (gep) {
                VisitResult rightVal = visit(node->data.assign.right);
                if (!rightVal.value) return VisitResult();
                if (!varName.empty() && varName != "argv" && !typeHelper.getArrayTypeInfo(varName)) {
                    typeHelper.registerArrayType(varName, rightVal.value->getType(), -1);
                    elemType = rightVal.value->getType();
                    gep = builder.CreateInBoundsGEP(elemType, targRes.value, idxVal, "arr_index_ptr");
                }
                ValueType vt = typeHelper.getValueTypeFromType(elemType);
                Value* casted = typeHelper.castValue(builder, rightVal.value, rightVal.type, vt);
                builder.CreateStore(casted, gep);
                return VisitResult(casted, vt);
            }
        }

        return VisitResult();
    }
    
    VisitResult visitStructAssign(ASTNode* node) {
        if (!node || node->type != AST_ASSIGN) return VisitResult();
        
        ASTNode* left = node->data.assign.left;
        ASTNode* right = node->data.assign.right;
        
        if (!left || left->type != AST_IDENTIFIER) return VisitResult();
        if (!right || right->type != AST_STRUCT_LITERAL) return VisitResult();
        
        std::string varName(left->data.identifier.name);
        
        ASTNode* typeNameNode = right->data.struct_literal.type_name;
        if (!typeNameNode) return VisitResult();

        Type* rawType = typeHelper.getTypeFromTypeNode(typeNameNode);
        StructType* structType = rawType && rawType->isStructTy() ? cast<StructType>(rawType) : nullptr;
        if (!structType) return VisitResult();
        
        Function* func = getCurrentFunction();
        if (!func) {
            func = module->getFunction("main");
            if (!func) {
                createDefaultMain();
                func = module->getFunction("main");
            }
            if (func) {
                builder.SetInsertPoint(&func->getEntryBlock());
            }
        }
        
        if (!func) return VisitResult();
        BasicBlock* entryBB = &func->getEntryBlock();
        BasicBlock* savedBB = builder.GetInsertBlock();
        
        IRBuilder<> tempBuilder(entryBB, entryBB->begin());
        AllocaInst* alloc = tempBuilder.CreateAlloca(structType, nullptr, varName);
        
        if (savedBB) {
            builder.SetInsertPoint(savedBB);
        }
        
        scopeManager.defineVariable(varName, alloc);
        initStructLiteral(alloc, right);

        ASTNode* initFields = right->data.struct_literal.fields;
        if (initFields && initFields->type == AST_EXPRESSION_LIST) {
            int initCount = initFields->data.expression_list.expression_count;
            for (int i = 0; i < initCount; i++) {
                ASTNode* f = initFields->data.expression_list.expressions[i];
                if (!f || f->type != AST_ASSIGN || !f->data.assign.left || !f->data.assign.right) continue;
                ASTNode* lhs = f->data.assign.left;
                ASTNode* rhs = f->data.assign.right;
                if (lhs->type != AST_IDENTIFIER || !lhs->data.identifier.name) continue;
                if (rhs->type != AST_EXPRESSION_LIST) continue;
                std::string key = varName + "." + std::string(lhs->data.identifier.name);
                memberArrayLengthHints[key] = rhs->data.expression_list.expression_count;
                if (rhs->data.expression_list.expression_count > 0) {
                    ASTNode* first = rhs->data.expression_list.expressions[0];
                    if (first && first->type == AST_EXPRESSION_LIST) {
                        memberNestedArrayLengthHints[key] = first->data.expression_list.expression_count;
                    }
                }
            }
        }

        return VisitResult(alloc, ValueType::POINTER, structType);
    }
    
    void initStructLiteral(AllocaInst* structAlloc, ASTNode* node) {
        if (!node || node->type != AST_STRUCT_LITERAL) return;
        
        ASTNode* typeName = node->data.struct_literal.type_name;
        ASTNode* fields = node->data.struct_literal.fields;
        if (!typeName) return;
        
        std::string structName;
        if (typeName->type == AST_IDENTIFIER && typeName->data.identifier.name) {
            structName = typeName->data.identifier.name;
        } else if (typeName->type == AST_TYPE_APP &&
                   typeName->data.type_app.ctor &&
                   typeName->data.type_app.ctor->type == AST_IDENTIFIER &&
                   typeName->data.type_app.ctor->data.identifier.name) {
            structName = typeName->data.type_app.ctor->data.identifier.name;
        } else {
            return;
        }
        
        StructType* structType = cast_or_null<StructType>(structAlloc->getAllocatedType());
        if (!structType) return;
        std::string mangledName(structType->getName());
        auto* fieldInfo = typeHelper.getStructFields(mangledName);
        if (!fieldInfo) fieldInfo = typeHelper.getStructFields(structName);
        if (!fieldInfo) return;
        
        if (!fields || fields->type != AST_EXPRESSION_LIST) return;
        
        int fieldCount = fields->data.expression_list.expression_count;
        for (size_t i = 0; i < fieldInfo->size(); i++) {
            const auto& field = (*fieldInfo)[i];
            std::string fieldName = field.first;
            Type* expectedType = field.second;
            
            Value* fieldPtr = builder.CreateStructGEP(structType, structAlloc, i, fieldName);
            Value* defaultValue = nullptr;
            
            if (expectedType->isPointerTy()) {
                defaultValue = ConstantPointerNull::get(cast<PointerType>(expectedType));
            } else if (expectedType->isIntegerTy()) {
                defaultValue = ConstantInt::get(expectedType, 0);
            } else if (expectedType->isFloatTy()) {
                defaultValue = ConstantFP::get(expectedType, 0.0);
            } else if (expectedType->isDoubleTy()) {
                defaultValue = ConstantFP::get(expectedType, 0.0);
            } else if (expectedType->isStructTy()) {
                defaultValue = Constant::getNullValue(expectedType);
            } else {
                continue;
            }

            builder.CreateStore(defaultValue, fieldPtr);
        }
        for (int i = 0; i < fieldCount && i < (int)fieldInfo->size(); i++) {
            ASTNode* fieldExpr = fields->data.expression_list.expressions[i];
            
            if (fieldExpr && fieldExpr->type == AST_ASSIGN) {
                ASTNode* fieldNameNode = fieldExpr->data.assign.left;
                ASTNode* exprNode = fieldExpr->data.assign.right;
                
                if (fieldNameNode && fieldNameNode->type == AST_IDENTIFIER && exprNode) {
                    std::string fieldName(fieldNameNode->data.identifier.name);
                    
                    int idx = -1;
                    for (size_t j = 0; j < fieldInfo->size(); j++) {
                        if ((*fieldInfo)[j].first == fieldName) {
                            idx = j;
                            break;
                        }
                    }
                    
                    if (idx >= 0) {
                        VisitResult fieldVal = visit(exprNode);
                        if (fieldVal.value) {
                            Value* fieldPtr = builder.CreateStructGEP(structType, structAlloc, idx, fieldName);
                            Type* expectedType = (*fieldInfo)[idx].second;
                            
                            if (expectedType->isStructTy()) {
                                Value* toStore = nullptr;
                                if (fieldVal.value->getType()->isPointerTy()) {
                                    toStore = builder.CreateLoad(expectedType, fieldVal.value, fieldName + "_val");
                                } else if (fieldVal.value->getType() == expectedType) {
                                    toStore = fieldVal.value;
                                }
                                if (toStore) builder.CreateStore(toStore, fieldPtr);
                            } else if (expectedType->isPointerTy() && fieldVal.value->getType()->isPointerTy()) {
                                Value* storeVal = fieldVal.value;
                                if (storeVal->getType() != expectedType) {
                                    storeVal = builder.CreateBitCast(storeVal, expectedType, "field_ptr_cast");
                                }
                                builder.CreateStore(storeVal, fieldPtr);
                            } else {
                                ValueType expectedValueType = typeHelper.getValueTypeFromType(expectedType);
                                Value* castedVal = typeHelper.castValue(builder, fieldVal.value, 
                                                                       fieldVal.type, expectedValueType);
                                builder.CreateStore(castedVal, fieldPtr);
                            }
                        }
                    }
                }
            }
        }
    }
    
    VisitResult visitMemberAssign(ASTNode* node) {
        if (!node || node->type != AST_ASSIGN) return VisitResult();
        
        ASTNode* left = node->data.assign.left;
        ASTNode* right = node->data.assign.right;
        if (!left || left->type != AST_MEMBER_ACCESS) return VisitResult();
        
        ASTNode* object = left->data.member_access.object;
        ASTNode* field = left->data.member_access.field;
        if (!object || !field) return VisitResult();
        
        VisitResult objectRes = visit(object);
        if (!objectRes.value) return VisitResult();
        
        if (!objectRes.value->getType()->isPointerTy()) return VisitResult();
        if (field->type != AST_IDENTIFIER) return VisitResult();
        
        std::string fieldName(field->data.identifier.name);
        
        StructType* structType = nullptr;
        Value* basePtr = nullptr;
        
        if (objectRes.structType) {
            structType = objectRes.structType;
            basePtr = objectRes.value;
        } else if (AllocaInst* alloc = dyn_cast<AllocaInst>(objectRes.value)) {
            Type* allocatedType = getActualType(alloc);
            if (allocatedType->isStructTy()) {
                structType = cast<StructType>(allocatedType);
                basePtr = alloc;
            }
        } else if (LoadInst* load = dyn_cast<LoadInst>(objectRes.value)) {
            Value* ptr = load->getPointerOperand();
            if (ptr->getType()->isPointerTy()) {
                if (AllocaInst* allocPtr = dyn_cast<AllocaInst>(ptr)) {
                    Type* allocatedType = getActualType(allocPtr);
                    if (allocatedType->isStructTy()) {
                        structType = cast<StructType>(allocatedType);
                        basePtr = ptr;
                    }
                }
            }
        }
        
        if (!structType || !basePtr) {
            llvm::errs() << "Error: Cannot assign to member '" << fieldName 
                        << "' of non-struct type\n";
            return VisitResult();
        }
        
        std::string structName = structType->getName().str();
        int idx = typeHelper.getFieldIndex(structName, fieldName);
        if (idx < 0) {
            llvm::errs() << "Error: Struct '" << structName 
                        << "' has no member named '" << fieldName << "'\n";
            return VisitResult();
        }
        
        Value* fieldPtr = builder.CreateStructGEP(structType, basePtr, idx, fieldName);
        
        VisitResult rightVal = visit(right);
        if (!rightVal.value) return VisitResult();
        
        Type* fieldType = structType->getElementType(idx);
        ValueType fieldValueType = typeHelper.getValueTypeFromType(fieldType);
        Value* castedVal = typeHelper.castValue(builder, rightVal.value, rightVal.type, fieldValueType);
        
        builder.CreateStore(castedVal, fieldPtr);
        
        return VisitResult(castedVal, fieldValueType, structType);
    }
    
    VisitResult visitProgram(ASTNode* node) {
        if (!node) return VisitResult();

        for (int i = 0; i < node->data.program.statement_count; i++) {
            ASTNode* stmt = node->data.program.statements[i];
            if (!stmt || stmt->type != AST_FUNCTION) continue;
            ASTNode* gparams = stmt->data.function.generic_params;
            if (gparams && gparams->type == AST_EXPRESSION_LIST && gparams->data.expression_list.expression_count > 0) {
                std::string name(stmt->data.function.name ? stmt->data.function.name : "");
                if (!name.empty()) {
                    genericFunctionTemplates[name] = stmt;
                    genericFunctionArity[name] = gparams->data.expression_list.expression_count;
                }
            }
        }

        for (int i = 0; i < node->data.program.statement_count; i++) {
            ASTNode* stmt = node->data.program.statements[i];
            if (stmt && stmt->type == AST_FUNCTION) {
                ASTNode* gparams = stmt->data.function.generic_params;
                if (gparams && gparams->type == AST_EXPRESSION_LIST && gparams->data.expression_list.expression_count > 0) {
                    continue;
                }
            }
            visit(node->data.program.statements[i]);
            BasicBlock* currentBB = builder.GetInsertBlock();
            if (currentBB && currentBB->getTerminator()) {
                break;
            }
        }
        
        return VisitResult();
    }

    VisitResult visitBreak(ASTNode* node) {
        (void)node;
        if (loopBreakTargets.empty()) {
            llvm::errs() << "Error: 'break' used outside of loop\n";
            return VisitResult();
        }

        BasicBlock* target = loopBreakTargets.back();
        builder.CreateBr(target);
        return VisitResult();
    }

    VisitResult visitContinue(ASTNode* node) {
        (void)node;
        if (loopContinueTargets.empty()) {
            llvm::errs() << "Error: 'continue' used outside of loop\n";
            return VisitResult();
        }

        BasicBlock* target = loopContinueTargets.back();
        builder.CreateBr(target);
        return VisitResult();
    }
    
    VisitResult visitIf(ASTNode* node) {
        Function* func = getCurrentFunction();
        if (!func) return VisitResult();
        
        VisitResult condRes = visit(node->data.if_stmt.condition);
        if (!condRes.value) return VisitResult();
        
        Value* cond = condRes.value;
        if (cond->getType() != Type::getInt1Ty(context)) {
            cond = builder.CreateICmpNE(cond, ConstantInt::get(cond->getType(), 0), "ifcond");
        }
        
        BasicBlock* thenBB = BasicBlock::Create(context, "then", func);
        BasicBlock* elseBB = BasicBlock::Create(context, "else");
        BasicBlock* mergeBB = BasicBlock::Create(context, "ifcont");
        
        builder.CreateCondBr(cond, thenBB, elseBB);
        
        builder.SetInsertPoint(thenBB);
        scopeManager.enterScope();
        visit(node->data.if_stmt.then_body);
        scopeManager.exitScope();
        thenBB = builder.GetInsertBlock();
        if (!thenBB->getTerminator()) {
            builder.CreateBr(mergeBB);
        }
        
        func->insert(func->end(), elseBB);
        func->insert(func->end(), mergeBB);
        
        builder.SetInsertPoint(elseBB);
        if (node->data.if_stmt.else_body) {
            scopeManager.enterScope();
            visit(node->data.if_stmt.else_body);
            scopeManager.exitScope();
        }
        elseBB = builder.GetInsertBlock();
        if (!elseBB->getTerminator()) {
            builder.CreateBr(mergeBB);
        }
        
        builder.SetInsertPoint(mergeBB);
        return VisitResult();
    }
    
    VisitResult visitWhile(ASTNode* node) {
        Function* func = getCurrentFunction();
        if (!func) return VisitResult();
        
        BasicBlock* condBB = BasicBlock::Create(context, "whilecond", func);
        BasicBlock* loopBB = BasicBlock::Create(context, "whilebody");
        BasicBlock* afterBB = BasicBlock::Create(context, "whilecont");
        
        builder.CreateBr(condBB);
        
        builder.SetInsertPoint(condBB);
        VisitResult condRes = visit(node->data.while_stmt.condition);
        if (!condRes.value) return VisitResult();
        
        Value* cond = condRes.value;
        if (cond->getType() != Type::getInt1Ty(context)) {
            cond = builder.CreateICmpNE(cond, ConstantInt::get(cond->getType(), 0), "whilecond");
        }
        func->insert(func->end(), loopBB);
        func->insert(func->end(), afterBB);
        builder.CreateCondBr(cond, loopBB, afterBB);
        builder.SetInsertPoint(loopBB);
        loopBreakTargets.push_back(afterBB);
        loopContinueTargets.push_back(condBB);
        scopeManager.enterScope();
        visit(node->data.while_stmt.body);
        scopeManager.exitScope();
        loopContinueTargets.pop_back();
        loopBreakTargets.pop_back();
        loopBB = builder.GetInsertBlock();
        if (!loopBB->getTerminator()) {
            builder.CreateBr(condBB);
        }
        
        builder.SetInsertPoint(afterBB);
        return VisitResult();
    }
    
    VisitResult visitFor(ASTNode* node) {
        if (!node) return VisitResult();
        
        Function* func = getCurrentFunction();
        if (!func) {
            llvm::errs() << "[ERROR] No current function in for loop\n";
            return VisitResult();
        }
        ASTNode* var_node = node->data.for_stmt.var;
        ASTNode* start_node = node->data.for_stmt.start;
        ASTNode* end_node = node->data.for_stmt.end;
        ASTNode* body_node = node->data.for_stmt.body;
        if (!var_node || !start_node || !body_node) return VisitResult();
        if (var_node->type != AST_IDENTIFIER) return VisitResult();
        std::string var_name(var_node->data.identifier.name);

        if (!end_node) {
            std::string iterableName;
            if (start_node->type == AST_IDENTIFIER && start_node->data.identifier.name) {
                iterableName = start_node->data.identifier.name;
            }

            VisitResult iterableRes = visit(start_node);
            if (!iterableRes.value || !iterableRes.value->getType()->isPointerTy()) {
                return VisitResult();
            }

            BasicBlock* entryBB = &func->getEntryBlock();
            BasicBlock* savedBB = builder.GetInsertBlock();
            IRBuilder<> tempBuilder(entryBB, entryBB->begin());

            Type* iterPtrTy = iterableRes.value->getType();
            AllocaInst* iterPtrAlloc = tempBuilder.CreateAlloca(iterPtrTy, nullptr, var_name + "__iterable");
            if (savedBB) builder.SetInsertPoint(savedBB);
            builder.CreateStore(iterableRes.value, iterPtrAlloc);

            Type* elemType = nullptr;
            if (!iterableName.empty()) {
                AllocaInst* iterAlloc = scopeManager.findVariable(iterableName);
                if (!iterAlloc) iterAlloc = findVariableInMain(iterableName);
                if (iterAlloc) {
                    Type* iterAllocType = getActualType(iterAlloc);
                    if (iterAllocType && iterAllocType->isPointerTy()) {
                        elemType = getPointerElementTypeSafely(dyn_cast<PointerType>(iterAllocType), iterableName);
                    }
                }
            }
            if (!elemType) {
                auto hintIt = pointerElementHints.find(iterableRes.value);
                if (hintIt != pointerElementHints.end() && hintIt->second) {
                    elemType = hintIt->second;
                }
            }
            if (!elemType) {
                elemType = Type::getInt8Ty(context);
            }
            ValueType elemVT = typeHelper.getValueTypeFromType(elemType);

            Value* iterLen = nullptr;
            if (!iterableName.empty()) {
                iterLen = getRuntimeArrayLengthValue(iterableName);
                if (!iterLen) {
                    if (auto* info = typeHelper.getArrayTypeInfo(iterableName)) {
                        if (info->second >= 0) {
                            iterLen = ConstantInt::get(Type::getInt32Ty(context), info->second);
                        }
                    }
                }
            }

            if (!iterLen) {
                if (start_node->type == AST_EXPRESSION_LIST) {
                    iterLen = ConstantInt::get(
                        Type::getInt32Ty(context),
                        start_node->data.expression_list.expression_count
                    );
                }
            }
            if (!iterLen) {
                Value* iterPtrNow = builder.CreateLoad(iterPtrTy, iterPtrAlloc, var_name + "__iterable_now");
                iterLen = emitLoadArrayLength(iterPtrNow, var_name + "__iterable_len");
            }

            AllocaInst* var_alloc = scopeManager.findVariable(var_name);
            if (!var_alloc) {
                BasicBlock* savedBB2 = builder.GetInsertBlock();
                IRBuilder<> tempBuilder2(entryBB, entryBB->begin());
                var_alloc = tempBuilder2.CreateAlloca(elemType, nullptr, var_name);
                if (savedBB2) builder.SetInsertPoint(savedBB2);
                scopeManager.defineVariable(var_name, var_alloc);
            }
            if (elemType->isPointerTy()) {
                typeHelper.registerArrayType(var_name, elemType, -1);
            }

            BasicBlock* savedBB3 = builder.GetInsertBlock();
            IRBuilder<> tempBuilder3(entryBB, entryBB->begin());
            AllocaInst* idx_alloc = tempBuilder3.CreateAlloca(Type::getInt32Ty(context), nullptr, var_name + "__idx");
            if (savedBB3) builder.SetInsertPoint(savedBB3);
            builder.CreateStore(ConstantInt::get(Type::getInt32Ty(context), 0), idx_alloc);

            BasicBlock* condBB = BasicBlock::Create(context, "forin_cond", func);
            BasicBlock* loopBB = BasicBlock::Create(context, "forin_body");
            BasicBlock* incBB = BasicBlock::Create(context, "forin_inc");
            BasicBlock* afterBB = BasicBlock::Create(context, "forin_cont");

            builder.CreateBr(condBB);
            builder.SetInsertPoint(condBB);
            Value* curIdx = builder.CreateLoad(Type::getInt32Ty(context), idx_alloc, var_name + "__idx_val");
            Value* cond = builder.CreateICmpSLT(curIdx, iterLen, "forin_cond_cmp");
            func->insert(func->end(), loopBB);
            func->insert(func->end(), incBB);
            func->insert(func->end(), afterBB);
            builder.CreateCondBr(cond, loopBB, afterBB);

            builder.SetInsertPoint(loopBB);
            Value* arrPtr = builder.CreateLoad(iterPtrTy, iterPtrAlloc, var_name + "__iter_ptr");
            Value* isNull = builder.CreateIsNull(arrPtr, "forin_arr_null");

            BasicBlock* bodyNullBB = BasicBlock::Create(context, "forin_body_null", func);
            BasicBlock* bodyLoadBB = BasicBlock::Create(context, "forin_body_load", func);
            BasicBlock* bodyJoinBB = BasicBlock::Create(context, "forin_body_join", func);
            builder.CreateCondBr(isNull, bodyNullBB, bodyLoadBB);

            builder.SetInsertPoint(bodyNullBB);
            Value* nullElem = Constant::getNullValue(elemType);
            builder.CreateBr(bodyJoinBB);

            builder.SetInsertPoint(bodyLoadBB);
            Value* idxForLoad = builder.CreateLoad(Type::getInt32Ty(context), idx_alloc, var_name + "__idx_cur");
            Value* elemPtr = builder.CreateInBoundsGEP(elemType, arrPtr, idxForLoad, "forin_elem_ptr");
            Value* loadedElem = builder.CreateLoad(elemType, elemPtr, "forin_elem");
            builder.CreateBr(bodyJoinBB);

            builder.SetInsertPoint(bodyJoinBB);
            PHINode* iterElem = builder.CreatePHI(elemType, 2, "forin_elem_phi");
            iterElem->addIncoming(nullElem, bodyNullBB);
            iterElem->addIncoming(loadedElem, bodyLoadBB);
            Value* castedElem = typeHelper.castValue(builder, iterElem, elemVT, elemVT);
            builder.CreateStore(castedElem, var_alloc);

            loopBreakTargets.push_back(afterBB);
            loopContinueTargets.push_back(incBB);
            scopeManager.enterScope();
            visit(body_node);
            scopeManager.exitScope();
            loopContinueTargets.pop_back();
            loopBreakTargets.pop_back();

            if (!builder.GetInsertBlock()->getTerminator()) {
                builder.CreateBr(incBB);
            }

            builder.SetInsertPoint(incBB);
            Value* curIdxForInc = builder.CreateLoad(Type::getInt32Ty(context), idx_alloc, var_name + "__idx_inc");
            Value* nextIdx = builder.CreateAdd(curIdxForInc, ConstantInt::get(Type::getInt32Ty(context), 1), "forin_idx_next");
            builder.CreateStore(nextIdx, idx_alloc);
            builder.CreateBr(condBB);

            builder.SetInsertPoint(afterBB);
            return VisitResult();
        }

        VisitResult start_val = visit(start_node);
        if (!start_val.value) return VisitResult();
        VisitResult end_val = visit(end_node);
        if (!end_val.value) {
            llvm::errs() << "[ERROR] Failed to evaluate for loop end condition\n";
            return VisitResult();
        }
        
        VIX_DEBUG_LOG << "[DEBUG] For loop end value type: " << *end_val.value->getType() << "\n";
        AllocaInst* var_alloc = scopeManager.findVariable(var_name);
        if (!var_alloc) {
            BasicBlock* entryBB = &func->getEntryBlock();
            BasicBlock* savedBB = builder.GetInsertBlock();
            
            Type* var_type = Type::getInt32Ty(context);
            IRBuilder<> tempBuilder(entryBB, entryBB->begin());
            var_alloc = tempBuilder.CreateAlloca(var_type, nullptr, var_name);
            
            if (savedBB) {
                builder.SetInsertPoint(savedBB);
            }
            
            scopeManager.defineVariable(var_name, var_alloc);
        }
        Value* start_val_casted = typeHelper.castValue(builder, start_val.value, start_val.type, ValueType::INT32);
        builder.CreateStore(start_val_casted, var_alloc);
        Value* end_val_casted = typeHelper.castValue(builder, end_val.value, end_val.type, ValueType::INT32);
        BasicBlock* condBB = BasicBlock::Create(context, "forcond", func);
        BasicBlock* loopBB = BasicBlock::Create(context, "forbody");
        BasicBlock* incBB = BasicBlock::Create(context, "forinc");
        BasicBlock* afterBB = BasicBlock::Create(context, "forcont");
        builder.CreateBr(condBB);
        builder.SetInsertPoint(condBB);
        Value* cur_val = builder.CreateLoad(Type::getInt32Ty(context), var_alloc, var_name);
        Value* descending = builder.CreateICmpSGT(start_val_casted, end_val_casted, "for_desc");
        Value* ascCond = builder.CreateICmpSLT(cur_val, end_val_casted, "forcond_asc");
        Value* descCond = builder.CreateICmpSGT(cur_val, end_val_casted, "forcond_desc");
        Value* cond = builder.CreateSelect(descending, descCond, ascCond, "forcond");
        VIX_DEBUG_LOG << "[DEBUG] for cond direction-aware (ascending/descending)\n";
        func->insert(func->end(), loopBB);
        func->insert(func->end(), incBB);
        func->insert(func->end(), afterBB);
        builder.CreateCondBr(cond, loopBB, afterBB);
        builder.SetInsertPoint(loopBB);
        loopBreakTargets.push_back(afterBB);
        loopContinueTargets.push_back(incBB);
        scopeManager.enterScope();
        visit(body_node);
        scopeManager.exitScope();
        loopContinueTargets.pop_back();
        loopBreakTargets.pop_back();
        if (!builder.GetInsertBlock()->getTerminator()) {
            builder.CreateBr(incBB);
        }
        builder.SetInsertPoint(incBB);
        Value* cur_val_for_inc = builder.CreateLoad(Type::getInt32Ty(context), var_alloc, var_name);
        Value* one_val = ConstantInt::get(Type::getInt32Ty(context), 1);
        Value* neg_one_val = ConstantInt::get(Type::getInt32Ty(context), -1);
        Value* step_val = builder.CreateSelect(descending, neg_one_val, one_val, "for_step");
        Value* new_val = builder.CreateAdd(cur_val_for_inc, step_val, "inc");
        builder.CreateStore(new_val, var_alloc);
        builder.CreateBr(condBB);
        
        builder.SetInsertPoint(afterBB);
        return VisitResult();
    }
    
    VisitResult visitFunction(ASTNode* node, const std::string* overrideName = nullptr) {
        std::string funcName = overrideName ? *overrideName : std::string(node->data.function.name);
        IRBuilder<>::InsertPoint callerIP = builder.saveIP();
        
        if (Function* existingFunc = module->getFunction(funcName)) {
            StructType* sretStructType = getStructSRetType(existingFunc);
            if (sretStructType) {
                return VisitResult(existingFunc, ValueType::POINTER, sretStructType);
            }
            return VisitResult(existingFunc, ValueType::POINTER);
        }
        
        std::vector<Type*> paramTypes;
        std::vector<std::string> paramNames;
        std::vector<ValueType> paramValueTypes;
        functionArrayParamPositions[funcName].clear();
        
        if (node->data.function.params) {
            if (node->data.function.params->type == AST_EXPRESSION_LIST) {
                int paramCount = node->data.function.params->data.expression_list.expression_count;
                int userParamIndex = 0;
                for (int i = 0; i < paramCount; i++) {
                    ASTNode* param = node->data.function.params->data.expression_list.expressions[i];
                    
                    if (param->type == AST_ASSIGN) {
                        ASTNode* left = param->data.assign.left;
                        ASTNode* right = param->data.assign.right;
                        if (left && left->type == AST_IDENTIFIER) {
                            std::string paramName(left->data.identifier.name);
                            paramNames.push_back(paramName);
                            ValueType paramType = ValueType::INT32;
                            bool isArrayParam = false;
                            Type* arrayElementType = Type::getInt32Ty(context);
                            int arrayElementCount = -1;
                            if (right && right->type == AST_IDENTIFIER) {
                                std::string typeName(right->data.identifier.name);
                                auto gbt = activeGenericTypeBindings.find(typeName);
                                if (gbt != activeGenericTypeBindings.end() && gbt->second) {
                                    Type* llvmParamType = gbt->second;
                                    if (llvmParamType->isStructTy()) {
                                        paramType = ValueType::POINTER;
                                        paramTypes.push_back(PointerType::getUnqual(llvmParamType));
                                    } else {
                                        paramType = typeHelper.getValueTypeFromType(llvmParamType);
                                        paramTypes.push_back(llvmParamType);
                                    }
                                } else if (StructType* structTy = typeHelper.getStructType(typeName)) {
                                    paramType = ValueType::POINTER;
                                    paramTypes.push_back(PointerType::getUnqual(structTy));
                                } else if (typeName == "ptr") {
                                    if (funcName == "main" && paramName == "argv") {
                                        paramType = ValueType::POINTER;
                                        paramTypes.push_back(PointerType::getUnqual(PointerType::getUnqual(Type::getInt8Ty(context))));
                                    } else {
                                        paramType = ValueType::POINTER;
                                        paramTypes.push_back(PointerType::getUnqual(Type::getInt8Ty(context)));
                                        typeHelper.registerStringVariable(paramName);//未指明更具体类型的 ptr 参数，通常按字符串/字节指针处理
                                    }
                                } else if (typeName == "i32") {
                                    paramType = ValueType::INT32;
                                    paramTypes.push_back(Type::getInt32Ty(context));
                                } else if (typeName == "i64") {
                                    paramType = ValueType::INT64;
                                    paramTypes.push_back(Type::getInt64Ty(context));
                                } else if (typeName == "f32") {
                                    paramType = ValueType::FLOAT32;
                                    paramTypes.push_back(Type::getFloatTy(context));
                                } else if (typeName == "f64") {
                                    paramType = ValueType::FLOAT64;
                                    paramTypes.push_back(Type::getDoubleTy(context));
                                } else if (typeName == "str") {
                                    paramType = ValueType::STRING;
                                    paramTypes.push_back(PointerType::getUnqual(Type::getInt8Ty(context)));
                                } else {
                                    paramTypes.push_back(Type::getInt32Ty(context));
                                }
                            }
                            else if (right) {
                                paramType = typeHelper.fromTypeNode(right);
                                if (right->type == AST_TYPE_POINTER) {
                                    if (funcName == "main" && paramName == "argv") {
                                        paramTypes.push_back(PointerType::getUnqual(PointerType::getUnqual(Type::getInt8Ty(context))));
                                    } else {
                                        paramTypes.push_back(PointerType::getUnqual(Type::getInt8Ty(context)));
                                        typeHelper.registerArrayType(paramName, Type::getInt32Ty(context), -1);
                                    }
                                } else if (right->type == AST_TYPE_LIST || right->type == AST_TYPE_FIXED_SIZE_LIST) {
                                    Type* elemType = typeHelper.getArrayElementTypeFromNode(right);
                                    int elemCount = typeHelper.getArrayElementCountFromNode(right);
                                    paramType = ValueType::POINTER;
                                    paramTypes.push_back(PointerType::getUnqual(elemType));
                                    typeHelper.registerArrayType(paramName, elemType, elemCount > 0 ? elemCount : -1);
                                    isArrayParam = true;
                                    arrayElementType = elemType;
                                    arrayElementCount = (elemCount > 0 ? elemCount : -1);
                                } else {
                                    Type* llvmParamType = typeHelper.getTypeFromTypeNode(right);
                                    if (llvmParamType && llvmParamType->isStructTy()) {
                                        paramType = ValueType::POINTER;
                                        paramTypes.push_back(PointerType::getUnqual(llvmParamType));
                                    } else {
                                        paramType = typeHelper.getValueTypeFromType(llvmParamType);
                                        paramTypes.push_back(llvmParamType);
                                    }
                                }
                            } else {
                                paramTypes.push_back(Type::getInt32Ty(context));
                            }
                            paramValueTypes.push_back(paramType);
                            if (paramType == ValueType::STRING) {
                                typeHelper.registerStringVariable(paramName);
                            }

                            if (isArrayParam) {
                                functionArrayParamPositions[funcName].push_back(userParamIndex);
                                paramNames.push_back(paramName + "__len");
                                paramTypes.push_back(Type::getInt32Ty(context));
                                paramValueTypes.push_back(ValueType::INT32);
                                typeHelper.registerArrayType(paramName, arrayElementType, arrayElementCount);
                                typeHelper.registerVariableArraySize(paramName, arrayElementCount);
                            }

                            userParamIndex++;
                        }
                    } else if (param->type == AST_IDENTIFIER) {
                        std::string paramName(param->data.identifier.name);
                        paramNames.push_back(paramName);
                        paramValueTypes.push_back(ValueType::INT32);
                        paramTypes.push_back(Type::getInt32Ty(context));
                        userParamIndex++;
                    }
                }
            }
        }
        typeHelper.setGenericTypeBindings(activeGenericTypeBindings);

        Type* logicalReturnType = Type::getVoidTy(context);
        ValueType returnValueType = ValueType::VOID;
        if (node->data.function.return_type) {
            logicalReturnType = typeHelper.getTypeFromTypeNode(node->data.function.return_type);
            returnValueType = typeHelper.getValueTypeFromType(logicalReturnType);
        }

        StructType* logicalReturnStructType = nullptr;
        bool useStructSRet = logicalReturnType && logicalReturnType->isStructTy();
        Type* abiReturnType = logicalReturnType;
        functionArrayReturnElementTypes.erase(funcName);
        if (node->data.function.return_type && node->data.function.return_type->type == AST_TYPE_LIST) {
            Type* elemType = typeHelper.getArrayElementTypeFromNode(node->data.function.return_type);
            functionArrayReturnElementTypes[funcName] = elemType;
        }
        if (useStructSRet) {
            logicalReturnStructType = cast<StructType>(logicalReturnType);
            abiReturnType = Type::getVoidTy(context);
            paramTypes.insert(paramTypes.begin(), PointerType::getUnqual(logicalReturnStructType));
        }

        bool isVarArg = node->data.function.vararg == 1;
        FunctionType* funcType = FunctionType::get(abiReturnType, paramTypes, isVarArg);
        Function* func = Function::Create(funcType, Function::ExternalLinkage, funcName, module.get());
        if (useStructSRet && logicalReturnStructType) {
            registerStructSRetFunction(funcName, func, logicalReturnStructType);
            func->addParamAttr(0, Attribute::getWithStructRetType(context, logicalReturnStructType));
            func->addParamAttr(0, Attribute::NoAlias);
        }
        auto fit = sourceAttrs.functionAttrs.find(funcName);
        if (fit != sourceAttrs.functionAttrs.end()) {
            if (!fit->second.section.empty()) {
                func->setSection(fit->second.section);
            }
            if (fit->second.exported) {
                func->setLinkage(GlobalValue::ExternalLinkage);
            }
        }//对于声明但未定义的外部函数，直接返回函数对象，不生成函数体
        if (node->data.function.is_extern && node->data.function.body == NULL) {
            return VisitResult(func, ValueType::POINTER, logicalReturnStructType);
        }
        
        if (funcName == "main") {
            mainFunctionCreated = true;
        }
        Function* prevFunc = scopeManager.getCurrentFunction();
        scopeManager.setCurrentFunction(func);
        BasicBlock* entryBB = BasicBlock::Create(context, "entry", func);
        builder.SetInsertPoint(entryBB);
        scopeManager.enterScope();
        unsigned idx = 0;
        unsigned userIdx = 0;
        for (auto& arg : func->args()) {
            if (useStructSRet && idx == 0) {
                arg.setName("__sret");
                ++idx;
                continue;
            }

            if (userIdx < paramNames.size()) {
                arg.setName(paramNames[userIdx]);
                Type* paramAllocType = arg.getType();
                
                AllocaInst* alloc = builder.CreateAlloca(paramAllocType, nullptr, paramNames[userIdx]);
                builder.CreateStore(&arg, alloc);
                scopeManager.defineVariable(paramNames[userIdx], alloc);
                if (userIdx < paramValueTypes.size() && paramValueTypes[userIdx] == ValueType::STRING) {
                    typeHelper.registerStringVariable(paramNames[userIdx]);
                }
                userIdx++;
            }
            ++idx;
        }
        
        ASTNode* body = node->data.function.body;
        VisitResult lastBodyResult;
        if (body) {
            if (body->type == AST_PROGRAM) {
                for (int i = 0; i < body->data.program.statement_count; i++) {
                    lastBodyResult = visit(body->data.program.statements[i]);
                }
            } else {
                lastBodyResult = visit(body);
            }
        }

        if (!useStructSRet && !logicalReturnType->isVoidTy()) {
            BasicBlock* curBB = builder.GetInsertBlock();
            if (curBB && !curBB->getTerminator() && lastBodyResult.value) {
                ValueType expectedValueType = typeHelper.getValueTypeFromType(logicalReturnType);
                Value* retValue = typeHelper.castValue(builder, lastBodyResult.value, lastBodyResult.type, expectedValueType);
                if (logicalReturnType->isPointerTy() && retValue->getType()->isPointerTy() &&
                    retValue->getType() != logicalReturnType) {
                    retValue = builder.CreateBitCast(retValue, logicalReturnType, "ret_ptr_cast");
                }
                builder.CreateRet(retValue);
            }
        }

        if (useStructSRet) {
            BasicBlock* curBB = builder.GetInsertBlock();
            if (curBB && !curBB->getTerminator() && lastBodyResult.value && logicalReturnStructType) {
                Argument* sretArg = func->arg_begin();
                Value* sretPtr = sretArg;
                Type* expectSretPtrType = PointerType::getUnqual(logicalReturnStructType);
                if (sretPtr->getType() != expectSretPtrType) {
                    sretPtr = builder.CreateBitCast(sretPtr, expectSretPtrType, "fn_sret_ptrcast");
                }

                Value* retStructValue = nullptr;
                if (lastBodyResult.value->getType() == logicalReturnStructType) {
                    retStructValue = lastBodyResult.value;
                } else if (lastBodyResult.value->getType()->isPointerTy()) {
                    Value* srcPtr = lastBodyResult.value;
                    if (srcPtr->getType() != expectSretPtrType) {
                        srcPtr = builder.CreateBitCast(srcPtr, expectSretPtrType, "fn_sret_src_ptrcast");
                    }
                    retStructValue = builder.CreateLoad(logicalReturnStructType, srcPtr, "fn_sret_val");
                }

                if (retStructValue) {
                    builder.CreateStore(retStructValue, sretPtr);
                    builder.CreateRetVoid();
                }
            }
        }
        
        scopeManager.exitScope();
        BasicBlock* endBB = BasicBlock::Create(context, "func_end", func);
        std::vector<BasicBlock*> blocks;
        for (auto& BB : *func) {
            blocks.push_back(&BB);
        }
        
        for (BasicBlock* BB : blocks) {
            if (BB == endBB) continue;
            if (!BB->getTerminator()) {
                IRBuilder<> tmpBuilder(BB);
                tmpBuilder.CreateBr(endBB);
            }
        }
        builder.SetInsertPoint(endBB);
        if (useStructSRet || logicalReturnType->isVoidTy()) {
            builder.CreateRetVoid();
        } else {
            Value* defaultRetVal = nullptr;
            if (logicalReturnType->isIntegerTy()) {
                defaultRetVal = ConstantInt::get(logicalReturnType, 0);
            } else if (logicalReturnType->isFloatTy()) {
                defaultRetVal = ConstantFP::get(logicalReturnType, 0.0);
            } else if (logicalReturnType->isDoubleTy()) {
                defaultRetVal = ConstantFP::get(logicalReturnType, 0.0);
            } else if (logicalReturnType->isPointerTy()) {
                defaultRetVal = ConstantPointerNull::get(cast<PointerType>(logicalReturnType));
            } else {
                defaultRetVal = Constant::getNullValue(logicalReturnType);
            }
            builder.CreateRet(defaultRetVal);
        }
        scopeManager.setCurrentFunction(prevFunc);
        if (callerIP.isSet()) {
            builder.restoreIP(callerIP);
        } else {
            builder.ClearInsertionPoint();
        }
        return VisitResult(func, ValueType::POINTER, logicalReturnStructType);
    }
    
    VisitResult visitCall(ASTNode* node) {
        if (!node->data.call.func) return VisitResult();

        if (node->data.call.func->type == AST_MEMBER_ACCESS) {
            ASTNode* mem = node->data.call.func;
            ASTNode* objectNode = mem->data.member_access.object;
            ASTNode* fieldNode = mem->data.member_access.field;
            if (!objectNode || !fieldNode || fieldNode->type != AST_IDENTIFIER || !fieldNode->data.identifier.name) {
                return VisitResult();
            }

            std::string methodName(fieldNode->data.identifier.name);
            if (methodName == "push") {
                if (!node->data.call.args || node->data.call.args->type != AST_EXPRESSION_LIST ||
                    node->data.call.args->data.expression_list.expression_count != 1) {
                    llvm::errs() << "Error: push expects exactly one argument\n";
                    return VisitResult();
                }

                std::string objectName;
                if (objectNode->type == AST_IDENTIFIER && objectNode->data.identifier.name) {
                    objectName = objectNode->data.identifier.name;
                }

                VisitResult objectRes = visit(objectNode);
                if (!objectRes.value || !objectRes.value->getType()->isPointerTy()) {
                    return VisitResult();
                }

                ASTNode* argNode = node->data.call.args->data.expression_list.expressions[0];
                VisitResult argRes = visit(argNode);
                if (!argRes.value) return VisitResult();

                AllocaInst* objectAlloc = nullptr;
                Value* objectSlotPtr = nullptr;
                Type* objectSlotElemType = nullptr;
                if (!objectName.empty()) {
                    objectAlloc = scopeManager.findVariable(objectName);
                    if (!objectAlloc) objectAlloc = findVariableInMain(objectName);
                }

                if (!objectAlloc && objectNode->type == AST_INDEX) {
                    ASTNode* slotTarget = objectNode->data.index.target;
                    ASTNode* slotIndexExpr = objectNode->data.index.index;
                    VisitResult slotIdxRes = visit(slotIndexExpr);
                    VisitResult slotTargetRes = visit(slotTarget);
                    if (slotIdxRes.value && slotTargetRes.value && slotTargetRes.value->getType()->isPointerTy()) {
                        Value* slotIdxVal = slotIdxRes.value;
                        if (!slotIdxVal->getType()->isIntegerTy(32)) {
                            slotIdxVal = builder.CreateIntCast(slotIdxVal, Type::getInt32Ty(context), true, "push_slot_idxcast");
                        }

                        std::string slotTargetName;
                        if (slotTarget->type == AST_IDENTIFIER && slotTarget->data.identifier.name) {
                            slotTargetName = slotTarget->data.identifier.name;
                        }
                        auto hintIt = pointerElementHints.find(slotTargetRes.value);
                        if (hintIt != pointerElementHints.end() && hintIt->second) {
                            objectSlotElemType = hintIt->second;
                        } else {
                            objectSlotElemType = getPointerElementTypeSafely(
                                dyn_cast<PointerType>(slotTargetRes.value->getType()),
                                slotTargetName
                            );
                        }
                        objectSlotPtr = builder.CreateInBoundsGEP(objectSlotElemType, slotTargetRes.value, slotIdxVal, "push_slot_ptr");
                    }
                }

                Type* elemType = Type::getInt32Ty(context);
                if (objectAlloc) {
                    Type* allocType = getActualType(objectAlloc);
                    if (allocType && allocType->isPointerTy()) {
                        elemType = getPointerElementTypeSafely(dyn_cast<PointerType>(allocType), objectName);
                    }
                } else if (objectRes.value->getType()->isPointerTy()) {
                    auto objHintIt = pointerElementHints.find(objectRes.value);
                    if (objHintIt != pointerElementHints.end() && objHintIt->second) {
                        elemType = objHintIt->second;
                    } else if (argRes.value) {
                        elemType = argRes.value->getType();
                    }
                }

                ValueType elemVT = typeHelper.getValueTypeFromType(elemType);
                Value* argCast = typeHelper.castValue(builder, argRes.value, argRes.type, elemVT);
                if (argCast->getType() != elemType) {
                    if (argCast->getType()->isPointerTy() && elemType->isPointerTy()) {
                        argCast = builder.CreateBitCast(argCast, elemType, "push_arg_ptrcast");
                    } else if (argCast->getType()->isIntegerTy() && elemType->isIntegerTy()) {
                        argCast = builder.CreateIntCast(argCast, elemType, true, "push_arg_intcast");
                    }
                }

                std::string pushStateName = objectName;
                if (pushStateName.empty()) {
                    pushStateName = std::string("__tmp_push_") + std::to_string((uintptr_t)objectNode);
                }

                uint64_t elemBytes = 4;
                if (elemType->isIntegerTy(8)) elemBytes = 1;
                else if (elemType->isIntegerTy(64) || elemType->isDoubleTy() || elemType->isPointerTy()) elemBytes = 8;
                else if (elemType->isFloatTy()) elemBytes = 4;

                Value* oldPtr = objectRes.value;
                Type* targetPtrTy = PointerType::getUnqual(elemType);
                if (oldPtr->getType() != targetPtrTy) {
                    oldPtr = builder.CreateBitCast(oldPtr, targetPtrTy, "push_old_ptr_cast");
                }

                Value* oldLen = emitLoadArrayLength(oldPtr, pushStateName + "_old_len");
                Value* newLen = builder.CreateAdd(oldLen, ConstantInt::get(Type::getInt32Ty(context), 1), pushStateName + "__len_new");

                PointerType* i8PtrTy = PointerType::getUnqual(Type::getInt8Ty(context));
                Value* oldPtrI8 = builder.CreateBitCast(oldPtr, i8PtrTy, "push_old_i8");

                Value* oldPtrInt = builder.CreatePtrToInt(oldPtrI8, Type::getInt64Ty(context));
                Value* baseInt = builder.CreateSub(oldPtrInt, ConstantInt::get(Type::getInt64Ty(context), ARRAY_HEADER_BYTES));
                Value* isOldNull = builder.CreateIsNull(oldPtrI8);
                Value* baseNonNullI8 = builder.CreateIntToPtr(baseInt, i8PtrTy);
                Value* baseI8 = builder.CreateSelect(isOldNull,
                    ConstantPointerNull::get(i8PtrTy),
                    baseNonNullI8, "push_base_ptr");

                Value* headerSizeVal = ConstantInt::get(Type::getInt64Ty(context), ARRAY_HEADER_BYTES);
                Value* newDataBytes = builder.CreateMul(
                    builder.CreateSExt(newLen, Type::getInt64Ty(context)),
                    ConstantInt::get(Type::getInt64Ty(context), elemBytes),
                    "push_data_bytes");
                Value* totalBytes = builder.CreateAdd(headerSizeVal, newDataBytes, "push_total_bytes");

                Function* reallocFn = getOrCreateReallocFunction();
                Value* newBaseI8 = builder.CreateCall(reallocFn, {baseI8, totalBytes}, "push_realloc");

                Value* newLenPtr = builder.CreateBitCast(newBaseI8, PointerType::getUnqual(Type::getInt32Ty(context)));
                builder.CreateStore(newLen, newLenPtr);

                Value* newDataI8 = builder.CreateInBoundsGEP(Type::getInt8Ty(context), newBaseI8, headerSizeVal, "push_new_data_i8");
                Value* newPtr = builder.CreateBitCast(newDataI8, targetPtrTy, "push_new_ptr");

                Value* dstPtr = builder.CreateInBoundsGEP(elemType, newPtr, oldLen, "push_dst_ptr");
                builder.CreateStore(argCast, dstPtr);

                if (objectAlloc) {
                    Type* allocType = getActualType(objectAlloc);
                    Value* storePtr = newPtr;
                    if (allocType && allocType != targetPtrTy && allocType->isPointerTy()) {
                        storePtr = builder.CreateBitCast(newPtr, allocType, "push_store_ptr_cast");
                    }
                    builder.CreateStore(storePtr, objectAlloc);
                }

                if (objectSlotPtr && objectSlotElemType) {
                    Value* slotStorePtr = newPtr;
                    if (slotStorePtr->getType() != objectSlotElemType &&
                        slotStorePtr->getType()->isPointerTy() && objectSlotElemType->isPointerTy()) {
                        slotStorePtr = builder.CreateBitCast(slotStorePtr, objectSlotElemType, "push_slot_store_cast");
                    }
                    builder.CreateStore(slotStorePtr, objectSlotPtr);
                }

                if (!objectName.empty()) {
                    typeHelper.registerArrayType(objectName, elemType, -1);
                    typeHelper.registerVariableArraySize(objectName, -1);
                }

                return VisitResult(newPtr, ValueType::POINTER);
            }

            return VisitResult();
        }

        if (node->data.call.func->type != AST_IDENTIFIER) {
            VisitResult calleeRes = visit(node->data.call.func);
            if (calleeRes.value && calleeRes.value->getType()->isPointerTy()) {
                Type* hintRetTy = nullptr;
                if (Function* curFn = getCurrentFunction()) {
                    hintRetTy = curFn->getReturnType();
                }
                return emitFunctionPointerCall(calleeRes.value, node->data.call.args, hintRetTy);
            }
            return VisitResult();
        }
        
        std::string calleeName(node->data.call.func->data.identifier.name);

        if (calleeName == "stdin") {
            int stdinArgCount = node->data.call.args ?
                node->data.call.args->data.expression_list.expression_count : 0;
            if (stdinArgCount == 0) {
                Type* i8PtrTy = PointerType::getUnqual(Type::getInt8Ty(context));
                Function* fdopenFn = module->getFunction("fdopen");
                if (!fdopenFn) {
                    FunctionType* fdopenTy = FunctionType::get(
                        i8PtrTy,
                        {Type::getInt32Ty(context), i8PtrTy},
                        false
                    );
                    fdopenFn = Function::Create(
                        fdopenTy,
                        Function::ExternalLinkage,
                        "fdopen",
                        module.get()
                    );
                    fdopenFn->setCallingConv(CallingConv::C);
                }

                Value* modeStr = safeCreateGlobalStringPtr("r", "stdin_mode");
                Value* fdZero = ConstantInt::get(Type::getInt32Ty(context), 0);
                Value* stdinVal = builder.CreateCall(fdopenFn, {fdZero, modeStr}, "stdin_val");
                return VisitResult(stdinVal, ValueType::POINTER);
            }
        }

        std::vector<PrintfFormatSpec> printfSpecs;
        if ((calleeName == "printf" || calleeName == "sprintf" || calleeName == "snprintf") &&
            node->data.call.args &&
            node->data.call.args->type == AST_EXPRESSION_LIST &&
            node->data.call.args->data.expression_list.expression_count > 0) {
            ASTNode* fmtNode = node->data.call.args->data.expression_list.expressions[0];
            if (fmtNode && fmtNode->type == AST_STRING && fmtNode->data.string.value) {
                printfSpecs = parsePrintfFormatSpecs(fmtNode->data.string.value);
            }
        }

        if (isBuiltinUnionCtorName(calleeName) || isRegisteredUnionCtorName(calleeName)) {
            int argCount = node->data.call.args ?
                node->data.call.args->data.expression_list.expression_count : 0;

            if (calleeName == "None") {
                if (argCount != 0) {
                    llvm::errs() << "Error: None() does not accept arguments\n";
                    return VisitResult();
                }
                return VisitResult(ConstantPointerNull::get(PointerType::getUnqual(Type::getInt8Ty(context))), ValueType::POINTER);
            }

            if (isRegisteredUnionCtorName(calleeName) && !isBuiltinUnionCtorName(calleeName)) {
                int payload_count = vix_adt_ctor_payload_count(calleeName.c_str());
                if (payload_count > 0 && argCount == 1 && node->data.call.args &&
                    node->data.call.args->type == AST_EXPRESSION_LIST) {
                    VisitResult argRes = visit(node->data.call.args->data.expression_list.expressions[0]);
                    if (!argRes.value) return VisitResult();

                    int32_t tagValue = ctorTagValue(calleeName);
                    Type* i32Ty = Type::getInt32Ty(context);
                    Type* i8PtrTy = PointerType::getUnqual(Type::getInt8Ty(context));
                    StructType* adtStructTy = StructType::get(context, {i32Ty, i8PtrTy});

                    Function* func = getCurrentFunction();
                    if (!func) {
                        func = module->getFunction("main");
                        if (!func) { createDefaultMain(); func = module->getFunction("main"); }
                    }
                    if (!func) return VisitResult();

                    BasicBlock* savedBB = builder.GetInsertBlock();
                    IRBuilder<> tempBuilder(&func->getEntryBlock(), func->getEntryBlock().begin());
                    AllocaInst* adtAlloca = tempBuilder.CreateAlloca(adtStructTy, nullptr, "adt");
                    if (savedBB) builder.SetInsertPoint(savedBB);

                    Value* tagPtr = builder.CreateStructGEP(adtStructTy, adtAlloca, 0, "tag_ptr");
                    builder.CreateStore(ConstantInt::get(i32Ty, tagValue), tagPtr);

                    Value* payloadPtr = builder.CreateStructGEP(adtStructTy, adtAlloca, 1, "payload_ptr");
                    Value* payload = argRes.value;
                    if (!payload->getType()->isPointerTy()) {
                        payload = builder.CreateIntToPtr(
                            builder.CreateIntCast(payload, Type::getInt64Ty(context), true, "adt_payload_cast"),
                            i8PtrTy);
                    } else if (payload->getType() != i8PtrTy) {
                        payload = builder.CreateBitCast(payload, i8PtrTy, "adt_payload_bcast");
                    }
                    builder.CreateStore(payload, payloadPtr);

                    return VisitResult(adtAlloca, ValueType::POINTER, adtStructTy);
                }
                if (argCount != 0) {
                    llvm::errs() << "Error: " << calleeName << "() does not accept arguments\n";
                    return VisitResult();
                }
                return VisitResult(ConstantInt::get(Type::getInt32Ty(context), ctorTagValue(calleeName), true), ValueType::INT32);
            }

            if (argCount != 1 || !node->data.call.args || node->data.call.args->type != AST_EXPRESSION_LIST) {
                llvm::errs() << "Error: " << calleeName << "(...) expects one argument\n";
                return VisitResult();
            }

            VisitResult argRes = visit(node->data.call.args->data.expression_list.expressions[0]);
            if (!argRes.value) return VisitResult();

            if (calleeName == "Ok" || calleeName == "Err") {
                int32_t tagValue = (calleeName == "Err") ? 1 : 0;
                Type* i32Ty = Type::getInt32Ty(context);
                Type* i8PtrTy = PointerType::getUnqual(Type::getInt8Ty(context));
                StructType* adtStructTy = StructType::get(context, {i32Ty, i8PtrTy});

                Function* func = getCurrentFunction();
                if (!func) {
                    func = module->getFunction("main");
                    if (!func) { createDefaultMain(); func = module->getFunction("main"); }
                }
                if (!func) return VisitResult();

                BasicBlock* savedBB = builder.GetInsertBlock();
                IRBuilder<> tempBuilder(&func->getEntryBlock(), func->getEntryBlock().begin());
                AllocaInst* adtAlloca = tempBuilder.CreateAlloca(adtStructTy, nullptr, "adt");
                if (savedBB) builder.SetInsertPoint(savedBB);

                Value* tagPtr = builder.CreateStructGEP(adtStructTy, adtAlloca, 0, "tag_ptr");
                builder.CreateStore(ConstantInt::get(i32Ty, tagValue), tagPtr);

                Value* payloadPtr = builder.CreateStructGEP(adtStructTy, adtAlloca, 1, "payload_ptr");
                Value* payload = argRes.value;
                if (!payload->getType()->isPointerTy()) {
                    payload = builder.CreateIntToPtr(
                        builder.CreateIntCast(payload, Type::getInt64Ty(context), true, "adt_payload_cast"),
                        i8PtrTy);
                } else if (payload->getType() != i8PtrTy) {
                    payload = builder.CreateBitCast(payload, i8PtrTy, "adt_payload_bcast");
                }
                builder.CreateStore(payload, payloadPtr);

                return VisitResult(adtAlloca, ValueType::POINTER, adtStructTy);
            }

            return argRes;
        }

        if (node->data.call.type_args && node->data.call.type_args->type == AST_EXPRESSION_LIST) {
            int actualTypeArgCount = node->data.call.type_args->data.expression_list.expression_count;
            auto arityIt = genericFunctionArity.find(calleeName);
            if (arityIt != genericFunctionArity.end() && arityIt->second != actualTypeArgCount) {
                llvm::errs() << "Error: Generic function '" << calleeName << "' expects "
                             << arityIt->second << " type arguments but got " << actualTypeArgCount << "\n";
                return VisitResult();
            }

            Function* instFn = instantiateGenericFunction(calleeName, node->data.call.type_args);
            if (!instFn) {
                llvm::errs() << "Error: Failed to instantiate generic function '" << calleeName << "'\n";
                return VisitResult();
            }
            calleeName = mangleGenericFunctionName(calleeName, node->data.call.type_args);
        }

        Function* callee = module->getFunction(calleeName);
        if (!callee) {
            auto fit = genericFunctionTemplates.find(calleeName);
            if (fit != genericFunctionTemplates.end()) {
                Function* instFn = instantiateGenericFunction(calleeName, nullptr);
                if (instFn) {
                    calleeName = mangleGenericFunctionName(calleeName, nullptr);
                    callee = module->getFunction(calleeName);
                }
            }
        }
        if (!callee) {
            AllocaInst* fnPtrAlloc = scopeManager.findVariable(calleeName);
            if (!fnPtrAlloc) fnPtrAlloc = findVariableInMain(calleeName);
            if (fnPtrAlloc) {
                Type* allocType = getActualType(fnPtrAlloc);
                if (allocType && allocType->isPointerTy()) {
                    Value* fnPtr = builder.CreateLoad(allocType, fnPtrAlloc, calleeName + "_fnptr");
                    if (fnPtr && fnPtr->getType()->isPointerTy()) {
                        Type* hintRetTy = nullptr;
                        if (Function* curFn = getCurrentFunction()) {
                            hintRetTy = curFn->getReturnType();
                        }
                        return emitFunctionPointerCall(fnPtr, node->data.call.args, hintRetTy);
                    }
                }
            }
            llvm::errs() << "Error: Call to undefined function '" << calleeName << "'\n";
            return VisitResult();
        }
        
        int expectedParamCount = callee->getFunctionType()->getNumParams();
        int actualParamCount = node->data.call.args ? 
            node->data.call.args->data.expression_list.expression_count : 0;
        int hiddenArrayLenParamCount = 0;
        int hiddenSRetParamCount = usesStructSRet(callee) ? 1 : 0;
        auto arrayMetaIt = functionArrayParamPositions.find(calleeName);
        if (arrayMetaIt != functionArrayParamPositions.end()) {
            hiddenArrayLenParamCount = (int)arrayMetaIt->second.size();
        }
        int expectedUserParamCount = expectedParamCount - hiddenArrayLenParamCount - hiddenSRetParamCount;
        bool isVarArg = callee->getFunctionType()->isVarArg();
        bool isKnownVarArgFunc = (calleeName == "printf" || calleeName == "sprintf" ||
                                  calleeName == "snprintf" || calleeName == "scanf");
        
        if (!isVarArg && !isKnownVarArgFunc && actualParamCount != expectedUserParamCount) {
            llvm::errs() << "Error: Function '" << calleeName 
                        << "' expects " << expectedUserParamCount 
                        << " arguments but got " << actualParamCount << "\n";
            return VisitResult();
        }
        
        std::vector<Value*> args;
        int llvmParamIndex = 0;
        StructType* sretType = getStructSRetType(callee);
        AllocaInst* sretAlloc = nullptr;
        if (sretType) {
            Function* caller = getCurrentFunction();
            if (!caller) {
                caller = module->getFunction("main");
                if (!caller) {
                    createDefaultMain();
                    caller = module->getFunction("main");
                }
            }
            if (!caller) return VisitResult();

            BasicBlock* entryBB = &caller->getEntryBlock();
            BasicBlock* savedBB = builder.GetInsertBlock();
            IRBuilder<> tmpBuilder(entryBB, entryBB->begin());
            sretAlloc = tmpBuilder.CreateAlloca(sretType, nullptr, calleeName + "_ret");
            if (savedBB) {
                builder.SetInsertPoint(savedBB);
            }

            args.push_back(sretAlloc);
            llvmParamIndex = 1;
        }

        if (node->data.call.args) {
            for (int i = 0; i < actualParamCount; i++) {
                ASTNode* argNode = node->data.call.args->data.expression_list.expressions[i];
                VisitResult argRes;
                if (argNode && argNode->type == AST_FUNCTION) {
                    IRBuilder<>::InsertPoint savedIP = builder.saveIP();
                    argRes = visit(argNode);
                    if (savedIP.isSet()) {
                        builder.restoreIP(savedIP);
                    }
                } else {
                    argRes = visit(argNode);
                }
                if (!argRes.value) {
                    llvm::errs() << "Error: Failed to evaluate argument " << i 
                                << " for function '" << calleeName << "'\n";
                    return VisitResult();
                }
                if (llvmParamIndex < expectedParamCount || isVarArg || isKnownVarArgFunc) {
                    Type* expectedType = nullptr;
                    if (llvmParamIndex < expectedParamCount) {
                        expectedType = callee->getFunctionType()->getParamType(llvmParamIndex);
                    } else {
                        if (isKnownVarArgFunc) {
                            if ((calleeName == "printf" || calleeName == "sprintf" || calleeName == "snprintf") && i > 0) {
                                int fmtArgIndex = i - 1;
                                if (fmtArgIndex >= 0 && fmtArgIndex < (int)printfSpecs.size()) {
                                    const PrintfFormatSpec& spec = printfSpecs[fmtArgIndex];
                                    switch (spec.conv) {
                                        case 's':
                                        case 'p':
                                            expectedType = PointerType::getUnqual(Type::getInt8Ty(context));
                                            break;
                                        case 'f':
                                        case 'F':
                                        case 'e':
                                        case 'E':
                                        case 'g':
                                        case 'G':
                                        case 'a':
                                        case 'A':
                                            expectedType = Type::getDoubleTy(context);
                                            break;
                                        case 'd':
                                        case 'i':
                                        case 'u':
                                        case 'x':
                                        case 'X':
                                        case 'o':
                                        case 'c':
                                            expectedType = (spec.length >= 2)
                                                ? Type::getInt64Ty(context)
                                                : Type::getInt32Ty(context);
                                            break;
                                        default:
                                            break;
                                    }
                                }
                            }

                            if (argRes.value->getType()->isIntegerTy() && 
                                argRes.value->getType()->getIntegerBitWidth() < 32) {
                                if (!expectedType || expectedType->isIntegerTy()) {
                                    expectedType = Type::getInt32Ty(context);//提升到i32
                                }
                            }
                            if (!expectedType) {
                                expectedType = argRes.value->getType();
                            }
                        } else {
                            expectedType = argRes.value->getType();
                        }
                    }

                    if ((calleeName == "fgets" || calleeName == "gets") && i == 0 &&
                        argNode && argNode->type == AST_IDENTIFIER && argNode->data.identifier.name) {
                        std::string strBufName(argNode->data.identifier.name);
                        typeHelper.registerStringVariable(strBufName);
                        typeHelper.registerArrayType(strBufName, Type::getInt8Ty(context), -1);
                    }

                    if (calleeName == "strncpy" && i == 0 &&
                        argNode && argNode->type == AST_IDENTIFIER && argNode->data.identifier.name) {
                        std::string strBufName(argNode->data.identifier.name);
                        typeHelper.registerStringVariable(strBufName);
                        typeHelper.registerArrayType(strBufName, Type::getInt8Ty(context), -1);
                    }

                    if (expectedType && expectedType->isPointerTy() &&
                        (!argRes.value->getType()->isPointerTy()) &&
                        argNode && argNode->type == AST_INDEX &&
                        argNode->data.index.target && argNode->data.index.target->type == AST_IDENTIFIER &&
                        argNode->data.index.target->data.identifier.name) {
                        std::string indexedBaseName(argNode->data.index.target->data.identifier.name);
                        AllocaInst* indexedBaseAlloc = scopeManager.findVariable(indexedBaseName);
                        if (!indexedBaseAlloc) indexedBaseAlloc = findVariableInMain(indexedBaseName);
                        GlobalVariable* indexedBaseGlobal = nullptr;
                        if (!indexedBaseAlloc) {
                            indexedBaseGlobal = findGlobalVariable(indexedBaseName);
                        }

                        Value* indexedBasePtr = nullptr;
                        if (indexedBaseAlloc) {
                            Type* indexedAllocTy = getActualType(indexedBaseAlloc);
                            if (indexedAllocTy && indexedAllocTy->isPointerTy()) {
                                indexedBasePtr = builder.CreateLoad(indexedAllocTy, indexedBaseAlloc, indexedBaseName + "_ptr");
                            }
                        } else if (indexedBaseGlobal) {
                            Type* globalTy = indexedBaseGlobal->getValueType();
                            if (globalTy && globalTy->isPointerTy()) {
                                indexedBasePtr = builder.CreateLoad(globalTy, indexedBaseGlobal, indexedBaseName + "_ptr");
                            }
                        }

                        if (indexedBasePtr && indexedBasePtr->getType()->isPointerTy()) {
                            VisitResult reIdxRes = visit(argNode->data.index.index);
                            if (reIdxRes.value) {
                                Value* reIdxVal = reIdxRes.value;
                                if (!reIdxVal->getType()->isIntegerTy(32)) {
                                    reIdxVal = builder.CreateIntCast(reIdxVal, Type::getInt32Ty(context), true, "idxcast_printf");
                                }

                                Type* forcedElemType = PointerType::getUnqual(Type::getInt8Ty(context));
                                Value* forcedGep = builder.CreateInBoundsGEP(forcedElemType, indexedBasePtr, reIdxVal, "fmt_ptr_index_ptr");
                                Value* forcedLoaded = builder.CreateLoad(forcedElemType, forcedGep, "fmt_ptr_index_load");

                                argRes.value = forcedLoaded;
                                argRes.type = ValueType::POINTER;
                                typeHelper.registerArrayType(indexedBaseName, forcedElemType, -1);
                            }
                        }
                    }
                    
                    ValueType expectedValueType = typeHelper.getValueTypeFromType(expectedType);
                    
                    Value* arg = typeHelper.castValue(builder, argRes.value, argRes.type, expectedValueType);
                    if (expectedType && expectedType->isPointerTy() && arg->getType()->isPointerTy() &&
                        arg->getType() != expectedType) {
                        arg = builder.CreateBitCast(arg, expectedType, "arg_ptrcast");
                    }//对于已知的可变参数函数，like printf 就允许传入与参数类型不完全匹配但可转换的值 like: i8* 传递给 %s
                    args.push_back(arg);
                    llvmParamIndex++;

                    if (!isVarArg && !isKnownVarArgFunc && isArrayParamPosition(calleeName, i)) {
                        Value* lenVal = inferArrayLengthFromArgument(argNode);
                        Type* lenExpectedType = Type::getInt32Ty(context);
                        if (llvmParamIndex < expectedParamCount) {
                            lenExpectedType = callee->getFunctionType()->getParamType(llvmParamIndex);
                        }

                        if (!lenVal->getType()->isIntegerTy()) {
                            lenVal = typeHelper.castValue(builder, lenVal, ValueType::INT32, ValueType::INT32);
                        }
                        if (lenVal->getType() != lenExpectedType && lenExpectedType->isIntegerTy()) {
                            lenVal = builder.CreateIntCast(lenVal, lenExpectedType, false, "arr_len_arg_cast");
                        }
                        args.push_back(lenVal);
                        llvmParamIndex++;
                    }
                }
            }
        }
        
        CallInst* callInst = builder.CreateCall(callee, args);
        if (sretAlloc) {
            return VisitResult(sretAlloc, ValueType::POINTER, sretType);
        }
        if (!callee->getReturnType()->isVoidTy()) callInst->setName("calltmp");
        if (callee->getReturnType()->isPointerTy()) {
            auto retElemIt = functionArrayReturnElementTypes.find(calleeName);
            if (retElemIt != functionArrayReturnElementTypes.end() && retElemIt->second) {
                pointerElementHints[callInst] = retElemIt->second;
            }
        }
        ValueType returnType = typeHelper.getValueTypeFromType(callee->getReturnType());
        return VisitResult(callInst, returnType);
    }
    
    VisitResult visitReturn(ASTNode* node) {
        Function* currentFunc = getCurrentFunction();
        if (!currentFunc) return VisitResult();
        
        Type* expectedReturnType = currentFunc->getReturnType();
        StructType* sretType = getStructSRetType(currentFunc);

        if (sretType) {
            Argument* sretArg = currentFunc->arg_begin();
            Value* sretPtr = sretArg;
            Type* expectSretPtrType = PointerType::getUnqual(sretType);
            if (sretPtr->getType() != expectSretPtrType) {
                sretPtr = builder.CreateBitCast(sretPtr, expectSretPtrType, "sret_ptrcast");
            }

            if (node->data.return_stmt.expr) {
                VisitResult retVal = visit(node->data.return_stmt.expr);
                if (!retVal.value) return VisitResult();

                Value* retStructValue = nullptr;
                if (retVal.value->getType() == sretType) {
                    retStructValue = retVal.value;
                } else if (retVal.value->getType()->isPointerTy()) {
                    Value* srcPtr = retVal.value;
                    Type* expectSrcPtrType = PointerType::getUnqual(sretType);
                    if (srcPtr->getType() != expectSrcPtrType) {
                        srcPtr = builder.CreateBitCast(srcPtr, expectSrcPtrType, "ret_sret_src_ptrcast");
                    }
                    retStructValue = builder.CreateLoad(sretType, srcPtr, "ret_sret_val");
                }

                if (!retStructValue) {
                    return VisitResult();
                }

                builder.CreateStore(retStructValue, sretPtr);
                builder.CreateRetVoid();
                return VisitResult(sretPtr, ValueType::POINTER, sretType);
            }

            builder.CreateStore(Constant::getNullValue(sretType), sretPtr);
            builder.CreateRetVoid();
            return VisitResult(sretPtr, ValueType::POINTER, sretType);
        }
        
        if (node->data.return_stmt.expr) {
            VisitResult retVal = visit(node->data.return_stmt.expr);
            if (!retVal.value) return VisitResult();
            
            ValueType expectedValueType = typeHelper.getValueTypeFromType(expectedReturnType);
            Value* retValue = typeHelper.castValue(builder, retVal.value, retVal.type, expectedValueType);
            if (expectedReturnType->isPointerTy() && retValue->getType()->isPointerTy() &&
                retValue->getType() != expectedReturnType) {
                retValue = builder.CreateBitCast(retValue, expectedReturnType, "ret_ptr_cast");
            }
            builder.CreateRet(retValue);
            return VisitResult(retValue, expectedValueType);
        }
        
        if (expectedReturnType->isVoidTy()) {
            builder.CreateRetVoid();
        } else {
            builder.CreateRet(Constant::getNullValue(expectedReturnType));
        }
        return VisitResult(nullptr, ValueType::VOID);
    }
    
    VisitResult visitStructDef(ASTNode* node) {
        std::string structName(node->data.struct_def.name);
        if (typeHelper.getStructType(structName) || typeHelper.getStructTemplate(structName)) return VisitResult();
        
        ASTNode* fields = node->data.struct_def.fields;
        if (!fields || fields->type != AST_EXPRESSION_LIST) return VisitResult();

        if (node->data.struct_def.generic_params &&
            node->data.struct_def.generic_params->type == AST_EXPRESSION_LIST) {
            typeHelper.registerStructTemplate(structName, node);
            return VisitResult(nullptr, ValueType::VOID);
        }
        
        std::vector<Type*> fieldTypes;
        std::vector<std::pair<std::string, Type*>> fieldInfo;
        StructType* structType = StructType::create(context, structName);
        typeHelper.registerStructType(structName, structType, {});

        int fieldCount = fields->data.expression_list.expression_count;
        for (int i = 0; i < fieldCount; i++) {
            ASTNode* field = fields->data.expression_list.expressions[i];
            
            if (field->type == AST_ASSIGN) {
                ASTNode* left = field->data.assign.left;
                ASTNode* right = field->data.assign.right;
                
                if (left && left->type == AST_IDENTIFIER) {
                    std::string fieldName(left->data.identifier.name);
                    Type* fieldType = typeHelper.getTypeFromTypeNode(right);
                    if (!fieldType) fieldType = Type::getInt32Ty(context);
                    if (fieldType == structType) {
                        reportCodegenSemanticError(node, "self-recursive struct fields must use pointer type");
                        return VisitResult();
                    }
                    fieldTypes.push_back(fieldType);
                    fieldInfo.push_back({fieldName, fieldType});
                }
            }
            else if (field->type == AST_TYPE_INT32 || field->type == AST_TYPE_INT64 ||
                     field->type == AST_TYPE_FLOAT32 || field->type == AST_TYPE_FLOAT64 ||
                     field->type == AST_TYPE_STRING) {
                char defaultName[32];
                snprintf(defaultName, sizeof(defaultName), "field%d", i);
                std::string fieldName(defaultName);
                Type* fieldType = typeHelper.getTypeFromTypeNode(field);
                fieldTypes.push_back(fieldType);
                fieldInfo.push_back({fieldName, fieldType});
            }
        }
        
        if (fieldTypes.empty()) return VisitResult();
        structType->setBody(fieldTypes, false);
        typeHelper.registerStructType(structName, structType, fieldInfo);
        return VisitResult(nullptr, ValueType::VOID);
    }
    
    VisitResult visitStructLiteral(ASTNode* node) {
        ASTNode* typeName = node->data.struct_literal.type_name;
        if (!typeName) return VisitResult();

        std::string structName;
        ASTNode* typeArgs = nullptr;
        if (typeName->type == AST_IDENTIFIER && typeName->data.identifier.name) {
            structName = typeName->data.identifier.name;
        } else if (typeName->type == AST_TYPE_APP &&
                   typeName->data.type_app.ctor &&
                   typeName->data.type_app.ctor->type == AST_IDENTIFIER &&
                   typeName->data.type_app.ctor->data.identifier.name) {
            structName = typeName->data.type_app.ctor->data.identifier.name;
            typeArgs = typeName->data.type_app.args;
        } else {
            return VisitResult();
        }

        Type* rawType = typeHelper.getTypeFromTypeNode(typeName);
        StructType* structType = rawType && rawType->isStructTy() ? cast<StructType>(rawType) : nullptr;
        if (!structType && typeArgs && typeArgs->type == AST_EXPRESSION_LIST) {
            if (Type* inst = typeHelper.getTypeFromTypeNode(typeName)) {
                if (inst->isStructTy()) {
                    structType = cast<StructType>(inst);
                }
            }
        }
        if (!structType) return VisitResult();
        
        Function* func = getCurrentFunction();
        if (!func) {
            func = module->getFunction("main");
            if (!func) {
                createDefaultMain();
                func = module->getFunction("main");
            }
        }
        
        if (!func) return VisitResult();
        BasicBlock* entryBB = &func->getEntryBlock();
        BasicBlock* savedBB = builder.GetInsertBlock();
        
        IRBuilder<> tempBuilder(entryBB, entryBB->begin());
        AllocaInst* structAlloc = tempBuilder.CreateAlloca(structType, nullptr, "tmp_struct");
        
        if (savedBB) {
            builder.SetInsertPoint(savedBB);
        }
        
        initStructLiteral(structAlloc, node);
        
        return VisitResult(structAlloc, ValueType::POINTER, structType);
    }
    
    VisitResult handleArrayLength(ASTNode* object) {
        
        if (object->type == AST_IDENTIFIER) {
            std::string varName(object->data.identifier.name);
            VIX_DEBUG_LOG << "[DEBUG] to geet length of variable: " << varName << "\n";

            if (Value* runtimeLen = getRuntimeArrayLengthValue(varName)) {
                VIX_DEBUG_LOG << "[DEBUG] Array length (runtime fat ptr): dynamic\n";
                return VisitResult(runtimeLen, ValueType::INT32);
            }
            
            AllocaInst* alloc = scopeManager.findVariable(varName);
            VIX_DEBUG_LOG << "[DEBUG] found in scopeMar: " << (alloc ? "yes" : "no") << "\n";
            
            if (!alloc) {
                alloc = findVariableInMain(varName);
                VIX_DEBUG_LOG << "[DEBUG] found in main: " << (alloc ? "yes" : "no") << "\n";
            }
            if (alloc) {
                Type* allocatedType = getActualType(alloc);
                VIX_DEBUG_LOG << "[DEBUG] type: " << *allocatedType << "\n";
                if (allocatedType && allocatedType->isArrayTy()) {
                    ArrayType* arrayType = cast<ArrayType>(allocatedType);
                    uint64_t numElements = arrayType->getNumElements();
                    Value* length = ConstantInt::get(Type::getInt32Ty(context), numElements);
                    VIX_DEBUG_LOG << "[DEBUG] Arr length (s): " << numElements << "\n";
                    return VisitResult(length, ValueType::INT32);
                }
                if (allocatedType && allocatedType->isPointerTy()) {
                    auto* arrayInfo = typeHelper.getArrayTypeInfo(varName);
                    if (arrayInfo) {
                        int elementCount = arrayInfo->second;
                        if (elementCount > 0) {
                            Value* length = ConstantInt::get(Type::getInt32Ty(context), elementCount);
                            VIX_DEBUG_LOG << "[DEBUG] Array length (r): " << elementCount << "\n";
                            return VisitResult(length, ValueType::INT32);
                        }
                    }
                    if (typeHelper.isStringVariable(varName)) {
                        Value* strPtr = builder.CreateLoad(allocatedType, alloc, varName);
                        CallInst* strlenCall = builder.CreateCall(strlenFunction, {strPtr}, "strlen");
                        Value* length = builder.CreateIntCast(strlenCall, Type::getInt32Ty(context), false, "len");
                        VIX_DEBUG_LOG << "[DEBUG] String length (strlen): dynamic\n";
                        return VisitResult(length, ValueType::INT32);
                    }
                    VIX_DEBUG_LOG << "[DEBUG] Unknown pointer type, returning 0\n";
                    Value* length = ConstantInt::get(Type::getInt32Ty(context), 0);
                    return VisitResult(length, ValueType::INT32);
                }
            }
            auto* arrayInfo = typeHelper.getArrayTypeInfo(varName);
            if (arrayInfo) {
                int elementCount = arrayInfo->second;
                Value* length = ConstantInt::get(Type::getInt32Ty(context), elementCount);
                VIX_DEBUG_LOG << "[DEBUG] Array length (type info): " << elementCount << "\n";
                return VisitResult(length, ValueType::INT32);
            }
            if (typeHelper.isStringVariable(varName)) {
                AllocaInst* varAlloc = scopeManager.findVariable(varName);
                if (!varAlloc) varAlloc = findVariableInMain(varName);
                
                if (varAlloc) {
                    Type* allocatedType = getActualType(varAlloc);
                    Value* strPtr = builder.CreateLoad(allocatedType, varAlloc, varName);
                    CallInst* strlenCall = builder.CreateCall(strlenFunction, {strPtr}, "strlen");
                    Value* length = builder.CreateIntCast(strlenCall, Type::getInt32Ty(context), false, "len");
                    VIX_DEBUG_LOG << "[DEBUG] String length (strlen from isStringVariable): dynamic\n";
                    return VisitResult(length, ValueType::INT32);
                }
            }
        }
        if (object->type == AST_EXPRESSION_LIST) {
            int count = object->data.expression_list.expression_count;
            Value* length = ConstantInt::get(Type::getInt32Ty(context), count);
            VIX_DEBUG_LOG << "[DEBUG] Literal length: " << count << "\n";
            return VisitResult(length, ValueType::INT32);
        }

        if (object->type == AST_MEMBER_ACCESS) {
            ASTNode* obj = object->data.member_access.object;
            ASTNode* field = object->data.member_access.field;
            if (obj && field && obj->type == AST_IDENTIFIER && field->type == AST_IDENTIFIER &&
                obj->data.identifier.name && field->data.identifier.name) {
                std::string key = std::string(obj->data.identifier.name) + "." + std::string(field->data.identifier.name);
                auto it = memberArrayLengthHints.find(key);
                if (it != memberArrayLengthHints.end()) {
                    return VisitResult(ConstantInt::get(Type::getInt32Ty(context), it->second), ValueType::INT32);
                }
            }
        }

        if (object->type == AST_INDEX) {
            ASTNode* outerTarget = object->data.index.target;
            if (outerTarget && outerTarget->type == AST_MEMBER_ACCESS) {
                ASTNode* obj = outerTarget->data.member_access.object;
                ASTNode* field = outerTarget->data.member_access.field;
                if (obj && field && obj->type == AST_IDENTIFIER && field->type == AST_IDENTIFIER &&
                    obj->data.identifier.name && field->data.identifier.name) {
                    std::string key = std::string(obj->data.identifier.name) + "." + std::string(field->data.identifier.name);
                    auto it = memberNestedArrayLengthHints.find(key);
                    if (it != memberNestedArrayLengthHints.end()) {
                        return VisitResult(ConstantInt::get(Type::getInt32Ty(context), it->second), ValueType::INT32);
                    }
                }
            }
        }
        
        {
            VisitResult objRes = visit(object);
            if (objRes.value && objRes.value->getType()->isPointerTy()) {
                Value* runtimeLen = emitLoadArrayLength(objRes.value, "member_arr_len");
                return VisitResult(runtimeLen, ValueType::INT32);
            }
        }

        Value* length = ConstantInt::get(Type::getInt32Ty(context), 0);
        return VisitResult(length, ValueType::INT32);
    }
    
    VisitResult visitMemberAccess(ASTNode* node) {
        ASTNode* object = node->data.member_access.object;
        ASTNode* field = node->data.member_access.field;
        if (!object || !field) return VisitResult();
        
        if (field->type == AST_IDENTIFIER) {
            std::string fieldName(field->data.identifier.name);
            if (fieldName == "length" || fieldName == "size") {
                return handleArrayLength(object);
            }
        }
        
        VisitResult objectRes = visit(object);
        if (!objectRes.value) return VisitResult();
        
        if (field->type != AST_IDENTIFIER) return VisitResult();
        
        std::string fieldName(field->data.identifier.name);

        bool numericField = !fieldName.empty();
        for (char c : fieldName) {
            if (c < '0' || c > '9') {
                numericField = false;
                break;
            }
        }
        if (numericField && objectRes.value->getType()->isPointerTy()) {
            long long tupleIndex = atoll(fieldName.c_str());
            if (tupleIndex < 0) tupleIndex = 0;

            if (objectRes.structType) {
                StructType* structTy = objectRes.structType;
                if ((unsigned)tupleIndex < structTy->getNumElements()) {
                    Value* fieldPtr = builder.CreateStructGEP(structTy, objectRes.value, (unsigned)tupleIndex, "struct_field");
                    Type* fieldType = structTy->getElementType((unsigned)tupleIndex);
                    Value* loaded = builder.CreateLoad(fieldType, fieldPtr, "struct_field_val");
                    if (fieldType->isPointerTy() && node && node->inferred_type) {
                        Type* inferredLLVM = getLLVMTypeFromTypeInfo(node->inferred_type);
                        if (inferredLLVM && inferredLLVM->isIntegerTy()) {
                            Value* intVal = builder.CreatePtrToInt(loaded, Type::getInt64Ty(context), "adt_payload_ptrtoint");
                            loaded = builder.CreateIntCast(intVal, inferredLLVM, true, "adt_payload_intcast");
                            return VisitResult(loaded, typeHelper.getValueTypeFromType(inferredLLVM));
                        }
                        if (inferredLLVM && inferredLLVM->isPointerTy() && inferredLLVM != fieldType) {
                            loaded = builder.CreateBitCast(loaded, inferredLLVM, "adt_payload_bcast");
                            return VisitResult(loaded, typeHelper.getValueTypeFromType(inferredLLVM));
                        }
                    }
                    return VisitResult(loaded, typeHelper.getValueTypeFromType(fieldType));
                }
            }

            Type* elemType = nullptr;
            auto hintIt = pointerElementHints.find(objectRes.value);
            if (hintIt != pointerElementHints.end() && hintIt->second) {
                elemType = hintIt->second;
            }
            if (!elemType) {
                std::string objName;
                if (object->type == AST_IDENTIFIER && object->data.identifier.name) {
                    objName = object->data.identifier.name;
                }
                elemType = getPointerElementTypeSafely(dyn_cast<PointerType>(objectRes.value->getType()), objName);
            }
            if (object && object->inferred_type &&
                (object->inferred_type->kind == TYPEINFO_APP ||
                 (object->inferred_type->kind == TYPEINFO_STRUCT && object->inferred_type->name &&
                  (vix_is_adt_definition(object->inferred_type->name) ||
                   strcmp(object->inferred_type->name, "Option") == 0 ||
                   strcmp(object->inferred_type->name, "Result") == 0)))) {
                StructType* adtStructTy = StructType::get(context, {Type::getInt32Ty(context), PointerType::getUnqual(Type::getInt8Ty(context))});
                elemType = adtStructTy;
            }
            if (!elemType) {
                elemType = Type::getInt8Ty(context);
            }
            if (!elemType) {
                std::string objName;
                if (object->type == AST_IDENTIFIER && object->data.identifier.name) {
                    objName = object->data.identifier.name;
                }
                elemType = getPointerElementTypeSafely(dyn_cast<PointerType>(objectRes.value->getType()), objName);
            }
            if (!elemType) {
                elemType = Type::getInt8Ty(context);
            }

            if (elemType->isStructTy()) {
                StructType* structTy = cast<StructType>(elemType);
                if ((unsigned)tupleIndex < structTy->getNumElements()) {
                    Value* fieldPtr = builder.CreateStructGEP(structTy, objectRes.value, (unsigned)tupleIndex, "struct_field");
                    Type* fieldType = structTy->getElementType((unsigned)tupleIndex);
                    Value* loaded = builder.CreateLoad(fieldType, fieldPtr, "struct_field_val");
                    if (fieldType->isPointerTy() && node && node->inferred_type) {
                        Type* inferredLLVM = getLLVMTypeFromTypeInfo(node->inferred_type);
                        if (inferredLLVM && inferredLLVM->isIntegerTy()) {
                            Value* intVal = builder.CreatePtrToInt(loaded, Type::getInt64Ty(context), "adt_payload_ptrtoint");
                            loaded = builder.CreateIntCast(intVal, inferredLLVM, true, "adt_payload_intcast");
                            return VisitResult(loaded, typeHelper.getValueTypeFromType(inferredLLVM));
                        }
                        if (inferredLLVM && inferredLLVM->isPointerTy() && inferredLLVM != fieldType) {
                            loaded = builder.CreateBitCast(loaded, inferredLLVM, "adt_payload_bcast");
                            return VisitResult(loaded, typeHelper.getValueTypeFromType(inferredLLVM));
                        }
                    }
                    return VisitResult(loaded, typeHelper.getValueTypeFromType(fieldType));
                }
            }

            Value* basePtr = objectRes.value;
            Type* expectedPtrType = PointerType::getUnqual(elemType);
            if (basePtr->getType() != expectedPtrType && basePtr->getType()->isPointerTy() && expectedPtrType->isPointerTy()) {
                basePtr = builder.CreateBitCast(basePtr, expectedPtrType, "tuple_base_cast");
            }

            Function* fn = builder.GetInsertBlock() ? builder.GetInsertBlock()->getParent() : nullptr;
            if (!fn) return VisitResult();

            BasicBlock* nullBB = BasicBlock::Create(context, "tuple_idx_null", fn);
            BasicBlock* loadBB = BasicBlock::Create(context, "tuple_idx_load", fn);
            BasicBlock* contBB = BasicBlock::Create(context, "tuple_idx_cont", fn);

            Value* isNull = builder.CreateIsNull(basePtr, "tuple_base_is_null");
            builder.CreateCondBr(isNull, nullBB, loadBB);

            builder.SetInsertPoint(nullBB);
            Value* nullVal = Constant::getNullValue(elemType);
            builder.CreateBr(contBB);

            builder.SetInsertPoint(loadBB);
            Value* idxVal = ConstantInt::get(Type::getInt32Ty(context), tupleIndex);
            Value* elemPtr = builder.CreateInBoundsGEP(elemType, basePtr, idxVal, "tuple_elem_ptr");
            Value* loaded = builder.CreateLoad(elemType, elemPtr, "tuple_elem");
            builder.CreateBr(contBB);

            builder.SetInsertPoint(contBB);
            PHINode* safeLoaded = builder.CreatePHI(elemType, 2, "tuple_elem_safe");
            safeLoaded->addIncoming(nullVal, nullBB);
            safeLoaded->addIncoming(loaded, loadBB);

            if (elemType->isPointerTy()) {
                pointerElementHints[safeLoaded] = Type::getInt8Ty(context);
            }
            return VisitResult(safeLoaded, typeHelper.getValueTypeFromType(elemType));
        }
        
        StructType* structType = nullptr;
        Value* basePtr = nullptr;

        if (objectRes.value->getType()->isIntegerTy()) {
            std::string inferredStructName;
            StructType* inferredStructType = typeHelper.inferStructTypeByFieldName(fieldName, &inferredStructName);
            if (inferredStructType) {
                structType = inferredStructType;
                Type* int64Ty = Type::getInt64Ty(context);
                Value* addrVal = objectRes.value;
                if (addrVal->getType() != int64Ty) {
                    if (!addrVal->getType()->isIntegerTy()) {
                        return VisitResult(ConstantInt::get(Type::getInt32Ty(context), 0), ValueType::INT32);
                    }
                    addrVal = builder.CreateIntCast(addrVal, int64Ty, true, "member_addr64");
                }
                basePtr = builder.CreateIntToPtr(addrVal, PointerType::getUnqual(structType), "member_obj_ptr");
            }
        }

        if (!objectRes.value->getType()->isPointerTy() && !basePtr) {
            llvm::errs() << "Error: Object is not a pointer\n";
            return VisitResult(ConstantInt::get(Type::getInt32Ty(context), 0), ValueType::INT32);
        }
        
        if (objectRes.structType) {
            structType = objectRes.structType;
            basePtr = objectRes.value;
        } else if (AllocaInst* alloc = dyn_cast<AllocaInst>(objectRes.value)) {
            Type* allocatedType = getActualType(alloc);
            if (allocatedType->isStructTy()) {
                structType = cast<StructType>(allocatedType);
                basePtr = alloc;
            }
        } else if (LoadInst* load = dyn_cast<LoadInst>(objectRes.value)) {
            Value* ptr = load->getPointerOperand();
            if (ptr->getType()->isPointerTy()) {
                if (AllocaInst* allocPtr = dyn_cast<AllocaInst>(ptr)) {
                    Type* allocatedType = getActualType(allocPtr);
                    if (allocatedType->isStructTy()) {
                        structType = cast<StructType>(allocatedType);
                        basePtr = ptr;
                    }
                }
            }
        }

        if (!structType && objectRes.value->getType()->isPointerTy()) {
            std::string inferredStructName;
            StructType* inferredStructType = typeHelper.inferStructTypeByFieldName(fieldName, &inferredStructName);
            if (inferredStructType) {
                structType = inferredStructType;
                Type* expectedPtrType = PointerType::getUnqual(structType);
                if (objectRes.value->getType() == expectedPtrType) {
                    basePtr = objectRes.value;
                } else {
                    basePtr = builder.CreateBitCast(objectRes.value, expectedPtrType, "struct_ptr_cast");
                }
            }
        }
        
        if (!structType || !basePtr) {
            llvm::errs() << "Error: Cannot access member '" << fieldName 
                        << "' - not a struct type\n";
            return VisitResult(ConstantInt::get(Type::getInt32Ty(context), 0), ValueType::INT32);
        }
        
        std::string structName = structType->getName().str();
        int idx = typeHelper.getFieldIndex(structName, fieldName);
        if (idx < 0) {
            llvm::errs() << "Error: Struct '" << structName 
                        << "' has no member named '" << fieldName << "'\n";
            return VisitResult(ConstantInt::get(Type::getInt32Ty(context), 0), ValueType::INT32);
        }
        
        Value* fieldPtr = builder.CreateStructGEP(structType, basePtr, idx, fieldName);

        Type* fieldType = structType->getElementType(idx);
        if (fieldType->isArrayTy()) {
            ArrayType* arrayType = cast<ArrayType>(fieldType);
            Type* elemType = arrayType->getElementType();
            Value* zero = ConstantInt::get(Type::getInt32Ty(context), 0);
            Value* elemPtr = builder.CreateInBoundsGEP(fieldType, fieldPtr, {zero, zero}, fieldName + "_ptr");

            if (elemType->isIntegerTy(8)) {
                return VisitResult(elemPtr, ValueType::STRING, structType);
            }
            return VisitResult(elemPtr, ValueType::POINTER, structType);
        }

        if (fieldType->isStructTy()) {
            StructType* nested = cast<StructType>(fieldType);
            return VisitResult(fieldPtr, ValueType::POINTER, nested);
        }

        Value* fieldVal = builder.CreateLoad(fieldType, fieldPtr, fieldName);
        ValueType resultType = typeHelper.getValueTypeFromType(fieldType);

        if (fieldType->isPointerTy() && node && node->inferred_type) {
            Type* inferredLLVM = getLLVMTypeFromTypeInfo(node->inferred_type);
            if (inferredLLVM && inferredLLVM->isIntegerTy() && fieldType->isPointerTy()) {
                Value* intVal = builder.CreatePtrToInt(fieldVal, Type::getInt64Ty(context), "adt_payload_ptrtoint");
                fieldVal = builder.CreateIntCast(intVal, inferredLLVM, true, "adt_payload_intcast");
                resultType = typeHelper.getValueTypeFromType(inferredLLVM);
                return VisitResult(fieldVal, resultType);
            }
            if (inferredLLVM && inferredLLVM->isPointerTy() && fieldType->isPointerTy() &&
                inferredLLVM != fieldType) {
                fieldVal = builder.CreateBitCast(fieldVal, inferredLLVM, "adt_payload_bcast");
                resultType = typeHelper.getValueTypeFromType(inferredLLVM);
                return VisitResult(fieldVal, resultType);
            }
        }

        if (fieldType->isPointerTy()) {
            Type* inferredElem = getInferredPointerElementType(node);
            if (inferredElem) {
                pointerElementHints[fieldVal] = inferredElem;
            } else if (fieldName == "scopes") {
                pointerElementHints[fieldVal] = PointerType::getUnqual(Type::getInt32Ty(context));
            } else {
                pointerElementHints[fieldVal] = Type::getInt32Ty(context);
            }
        }

        return VisitResult(fieldVal, resultType, structType);
    }
    
    VisitResult visitPrint(ASTNode* node) {
        if (!node || !node->data.print.expr) return VisitResult();
        
        if (!ensureValidInsertPoint()) {
            llvm::errs() << "Error: Cannot find valid insertion point for print\n";
            return VisitResult();
        }
        
        initPrintf();
        
        if (node->data.print.expr->type == AST_EXPRESSION_LIST) {
            ASTNode* list = node->data.print.expr;
            int exprCount = list->data.expression_list.expression_count;
            
            for (int i = 0; i < exprCount; i++) {
                ASTNode* expr = list->data.expression_list.expressions[i];
                VisitResult exprRes = visit(expr);
                
                if (!exprRes.value) {
                    Value* emptyStr = safeCreateGlobalStringPtr("", "empty_str");
                    Value* formatStr = safeCreateGlobalStringPtr("%s", "fmt_s");
                    builder.CreateCall(printfFunction, {formatStr, emptyStr});
                    continue;
                }
                
                Value* printValue = exprRes.value;
                ValueType printType = exprRes.type;
                Value* formatStr = nullptr;
                
                switch (printType) {
                    case ValueType::INT32:
                        formatStr = safeCreateGlobalStringPtr("%d", "fmt_i32");
                        builder.CreateCall(printfFunction, {formatStr, printValue});
                        break;
                        
                    case ValueType::INT8:
                        formatStr = safeCreateGlobalStringPtr("%c", "fmt_c");
                        builder.CreateCall(printfFunction, {formatStr, printValue});
                        break;
                        
                    case ValueType::INT64:
                        formatStr = safeCreateGlobalStringPtr("%lld", "fmt_i64");
                        builder.CreateCall(printfFunction, {formatStr, printValue});
                        break;
                        
                    case ValueType::FLOAT32:
                    case ValueType::FLOAT64:
                        formatStr = safeCreateGlobalStringPtr("%f", "fmt_f");
                        printValue = typeHelper.castValue(builder, printValue, printType, ValueType::FLOAT64);
                        builder.CreateCall(printfFunction, {formatStr, printValue});
                        break;
                        
                    case ValueType::STRING:
                        formatStr = safeCreateGlobalStringPtr("%s", "fmt_s");
                        builder.CreateCall(printfFunction, {formatStr, printValue});
                        break;
                        
                    case ValueType::BOOL:
                        formatStr = safeCreateGlobalStringPtr("%d", "fmt_b");
                        printValue = typeHelper.castValue(builder, printValue, printType, ValueType::INT32);
                        builder.CreateCall(printfFunction, {formatStr, printValue});
                        break;
                        
                    case ValueType::POINTER:
                        formatStr = safeCreateGlobalStringPtr("%p", "fmt_p");
                        builder.CreateCall(printfFunction, {formatStr, printValue});
                        break;
                        
                    case ValueType::ARRAY:
                        formatStr = safeCreateGlobalStringPtr("%p", "fmt_p");
                        builder.CreateCall(printfFunction, {formatStr, printValue});
                        break;
                        
                    default:
                        formatStr = safeCreateGlobalStringPtr("%d", "fmt_d");
                        if (printValue->getType()->isIntegerTy()) {
                            builder.CreateCall(printfFunction, {formatStr, printValue});
                        } else {
                            Value* defaultVal = ConstantInt::get(Type::getInt32Ty(context), 0);
                            builder.CreateCall(printfFunction, {formatStr, defaultVal});
                        }
                        break;
                }
            }
            
            Value* newline = safeCreateGlobalStringPtr("\n", "fmt_nl");
            if (newline) {
                builder.CreateCall(printfFunction, {newline});
            }
            
            return VisitResult(nullptr, ValueType::VOID);
        } else {
            VisitResult expr = visit(node->data.print.expr);
            
            if (!expr.value) {
                return VisitResult();
            }
            
            Value* printValue = expr.value;
            ValueType printType = expr.type;
            Value* formatStr = nullptr;
            
            switch (printType) {
                case ValueType::INT32:
                    formatStr = safeCreateGlobalStringPtr("%d\n", "fmt_i32_nl");
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::INT8:
                    formatStr = safeCreateGlobalStringPtr("%c\n", "fmt_c_nl");
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::INT64:
                    formatStr = safeCreateGlobalStringPtr("%lld\n", "fmt_i64_nl");
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::FLOAT32:
                case ValueType::FLOAT64:
                    formatStr = safeCreateGlobalStringPtr("%f\n", "fmt_f_nl");
                    printValue = typeHelper.castValue(builder, printValue, printType, ValueType::FLOAT64);
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::STRING:
                    formatStr = safeCreateGlobalStringPtr("%s\n", "fmt_s_nl");
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::BOOL:
                    formatStr = safeCreateGlobalStringPtr("%d\n", "fmt_b_nl");
                    printValue = typeHelper.castValue(builder, printValue, printType, ValueType::INT32);
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::POINTER:
                    formatStr = safeCreateGlobalStringPtr("%p\n", "fmt_p_nl");
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::ARRAY:
                    formatStr = safeCreateGlobalStringPtr("%p\n", "fmt_p_nl");
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                default:
                    formatStr = safeCreateGlobalStringPtr("%d\n", "fmt_d_nl");
                    if (printValue->getType()->isIntegerTy()) {
                        builder.CreateCall(printfFunction, {formatStr, printValue});
                    } else {
                        Value* defaultVal = ConstantInt::get(Type::getInt32Ty(context), 0);
                        builder.CreateCall(printfFunction, {formatStr, defaultVal});
                    }
                    break;
            }
            
            return VisitResult(printValue, printType);
        }
    }
    
    VisitResult visitInput(ASTNode* node) {
        (void)node;
        return VisitResult(ConstantInt::get(Type::getInt32Ty(context), 0), ValueType::INT32);
    }
    
    VisitResult visitToInt(ASTNode* node) {
        if (!node->data.toint.expr) return VisitResult();
        VisitResult expr = visit(node->data.toint.expr);
        if (!expr.value) return VisitResult();
        Value* val = typeHelper.castValue(builder, expr.value, expr.type, ValueType::INT32);
        return VisitResult(val, ValueType::INT32);
    }
    
    VisitResult visitToFloat(ASTNode* node) {
        if (!node->data.tofloat.expr) return VisitResult();
        VisitResult expr = visit(node->data.tofloat.expr);
        if (!expr.value) return VisitResult();
        Value* val = typeHelper.castValue(builder, expr.value, expr.type, ValueType::FLOAT64);
        return VisitResult(val, ValueType::FLOAT64);
    }

    VisitResult visitArrayLiteral(ASTNode* node) {
        if (!node || node->type != AST_EXPRESSION_LIST) return VisitResult();
        int count = node->data.expression_list.expression_count;
        if (count == 0) {
            return VisitResult(ConstantPointerNull::get(cast<PointerType>(typeHelper.getLLVMType(ValueType::POINTER))), ValueType::ARRAY);
        }

        std::vector<Value*> elems;
        elems.reserve(count);
        Type* elemType = nullptr;
        ValueType elemValueType = ValueType::INT32;

        for (int i = 0; i < count; i++) {
            ASTNode* e = node->data.expression_list.expressions[i];
            VisitResult r = visit(e);
            if (!r.value) return VisitResult();
            if (i == 0) {
                elemType = r.value->getType();
                elemValueType = typeHelper.getValueTypeFromType(elemType);
            }
            Value* v = r.value;
            if (typeHelper.getValueTypeFromType(v->getType()) != elemValueType) {
                v = typeHelper.castValue(builder, v, r.type, elemValueType);
            }
            elems.push_back(v);
        }

        if (!elemType) return VisitResult();

        uint64_t elemBytes = 4;
        if (elemType->isIntegerTy(8)) elemBytes = 1;
        else if (elemType->isIntegerTy(64) || elemType->isDoubleTy() || elemType->isPointerTy()) elemBytes = 8;
        else if (elemType->isFloatTy()) elemBytes = 4;

        Value* headerSizeVal = ConstantInt::get(Type::getInt64Ty(context), ARRAY_HEADER_BYTES);
        Value* dataBytes = ConstantInt::get(Type::getInt64Ty(context), (uint64_t)count * elemBytes);
        Value* totalBytes = builder.CreateAdd(headerSizeVal, dataBytes, "arr_total_bytes");
        Function* reallocFn = getOrCreateReallocFunction();
        Value* heapI8 = builder.CreateCall(
            reallocFn,
            {ConstantPointerNull::get(PointerType::getUnqual(Type::getInt8Ty(context))), totalBytes},
            "arr_heap"
        );
        Value* lenPtr = builder.CreateBitCast(heapI8, PointerType::getUnqual(Type::getInt32Ty(context)), "arr_len_ptr");
        builder.CreateStore(ConstantInt::get(Type::getInt32Ty(context), count), lenPtr);
        Value* dataI8 = builder.CreateInBoundsGEP(Type::getInt8Ty(context), heapI8, headerSizeVal, "arr_data_i8");
        Value* arrayPtr = builder.CreateBitCast(dataI8, PointerType::getUnqual(elemType), "arr_ptr");

        for (int i = 0; i < count; i++) {
            Value* idx = ConstantInt::get(Type::getInt32Ty(context), i);
            Value* gep = builder.CreateInBoundsGEP(elemType, arrayPtr, idx, "elem_ptr");
            builder.CreateStore(elems[i], gep);
        }

        pointerElementHints[arrayPtr] = elemType;
        return VisitResult(arrayPtr, ValueType::ARRAY);
    }

    VisitResult visitConst(ASTNode* node) {
        if (!node || !node->data.assign.left || !node->data.assign.right) return VisitResult();
        if (node->data.assign.left->type != AST_IDENTIFIER || !node->data.assign.left->data.identifier.name) return VisitResult();

        std::string name(node->data.assign.left->data.identifier.name);
        Function* curFunc = getCurrentFunction();

        if (!curFunc) {
            GlobalVariable* existing = module->getGlobalVariable(name, true);
            if (existing) {
                Type* vt = existing->getValueType();
                return VisitResult(existing, typeHelper.getValueTypeFromType(vt));
            }

            ValueType constType = ValueType::INT32;
            Constant* initConst = evaluateConstExpr(node->data.assign.right, &constType);
            Type* llvmTy = initConst ? initConst->getType() : Type::getInt32Ty(context);
            if (!initConst) initConst = Constant::getNullValue(llvmTy);

            GlobalVariable* gv = new GlobalVariable(
                *module,
                llvmTy,
                true,
                GlobalValue::ExternalLinkage,
                initConst,
                name
            );
            auto cit = sourceAttrs.constAttrs.find(name);
            if (cit != sourceAttrs.constAttrs.end()) {
                if (!cit->second.section.empty()) {
                    gv->setSection(cit->second.section);
                }
                if (cit->second.exported) {
                    gv->setLinkage(GlobalValue::ExternalLinkage);
                }
            }
            return VisitResult(gv, typeHelper.getValueTypeFromType(llvmTy));
        }

        VisitResult right = visit(node->data.assign.right);
        if (!right.value) return VisitResult();

        AllocaInst* alloc = scopeManager.findVariable(name);
        if (!alloc) {
            BasicBlock* entryBB = &curFunc->getEntryBlock();
            IRBuilder<> tempBuilder(entryBB, entryBB->begin());
            alloc = tempBuilder.CreateAlloca(right.value->getType(), nullptr, name);
            scopeManager.defineVariable(name, alloc);
        }

        Type* targetTy = alloc->getAllocatedType();
        Value* val = right.value;
        if (val->getType() != targetTy) {
            val = typeHelper.castValue(builder, right.value, right.type, typeHelper.getValueTypeFromType(targetTy));
        }
        builder.CreateStore(val, alloc);
        return VisitResult(val, typeHelper.getValueTypeFromType(targetTy));
    }

    VisitResult visitGlobal(ASTNode* node) {
        if (!node) return VisitResult();
        
        ASTNode* identifier = node->data.global_decl.identifier;
        ASTNode* typeNode = node->data.global_decl.type;
        ASTNode* initializer = node->data.global_decl.initializer;
        
        if (!identifier || identifier->type != AST_IDENTIFIER) {
            llvm::errs() << "Error: Global declaration must have an identifier\n";
            return VisitResult();
        }
        
        std::string varName(identifier->data.identifier.name);
        
        Type* globalType = nullptr;
        ValueType valueType = ValueType::INT32;
        
        if (typeNode) {
            valueType = typeHelper.fromTypeNode(typeNode);
            globalType = typeHelper.getLLVMType(valueType);
        } else if (initializer) {
            VisitResult initResult = visit(initializer);
            if (initResult.value) {
                globalType = initResult.value->getType();
                valueType = initResult.type;
            }
        }
        
        if (!globalType) {
            globalType = Type::getInt32Ty(context);
            valueType = ValueType::INT32;
        }
        
        Constant* initValue = nullptr;
        if (initializer) {
            VisitResult initResult = visit(initializer);
            if (initResult.value && isa<Constant>(initResult.value)) {
                Constant* rawInit = cast<Constant>(initResult.value);
                if (rawInit->getType() == globalType) {
                    initValue = rawInit;
                } else if (globalType->isPointerTy() && rawInit->getType()->isIntegerTy() &&
                           isa<ConstantInt>(rawInit) && cast<ConstantInt>(rawInit)->isZero()) {
                    initValue = ConstantPointerNull::get(cast<PointerType>(globalType));
                } else if (globalType->isIntegerTy() && rawInit->getType()->isIntegerTy()) {
                    int64_t iv = cast<ConstantInt>(rawInit)->getSExtValue();
                    initValue = ConstantInt::get(globalType, static_cast<uint64_t>(iv), true);
                }
            }
        }
        
        if (!initValue) {
            initValue = Constant::getNullValue(globalType);
        }
        
        GlobalVariable* globalVar = new GlobalVariable(
            *module,
            globalType,
            false,
            GlobalValue::ExternalLinkage,
            initValue,
            varName
        );
        
        return VisitResult(globalVar, valueType);
    }

    VisitResult visitIndex(ASTNode* node) {
        if (!node || !node->data.index.target || !node->data.index.index) return VisitResult();
        ASTNode* target = node->data.index.target;
        ASTNode* indexNode = node->data.index.index;
        
        
        if (target->type == AST_IDENTIFIER) {//处理字符串索引s[i]
            std::string varName(target->data.identifier.name);
            
            AllocaInst* alloc = scopeManager.findVariable(varName);
            if (!alloc) alloc = findVariableInMain(varName);
            
            if (alloc) {
                Type* allocatedType = getActualType(alloc);
                bool isStringType = false;
                auto* arrayInfo = typeHelper.getArrayTypeInfo(varName);
                if (arrayInfo && arrayInfo->first->isIntegerTy(8)) {
                    isStringType = true;
                }
                
                if (typeHelper.isStringVariable(varName)) {
                    isStringType = true;
                }
                
                if (allocatedType->isArrayTy()) {
                    ArrayType* arrType = cast<ArrayType>(allocatedType);
                    if (arrType->getElementType()->isIntegerTy(8)) {
                        isStringType = true;
                    }
                }
                if (allocatedType->isPointerTy() && typeHelper.isStringVariable(varName)) {
                    isStringType = true;
                }
                if (isStringType) {
                    VisitResult idxRes = visit(indexNode);
                    if (!idxRes.value) return VisitResult();
                    
                    Value* idxVal = idxRes.value;
                    if (!idxVal->getType()->isIntegerTy(32)) {
                        idxVal = builder.CreateIntCast(idxVal, Type::getInt32Ty(context), true, "idxcast");
                    }
                    
                    Value* charPtr = nullptr;
                    
                    if (allocatedType->isArrayTy()) {
                        Value* zero = ConstantInt::get(Type::getInt32Ty(context), 0);
                        charPtr = builder.CreateInBoundsGEP(
                            allocatedType, alloc, {zero, idxVal}, "char_ptr");
                    } else {
                        Value* strPtr = builder.CreateLoad(allocatedType, alloc, varName);
                        Type* pointeeType = nullptr;
                        if (allocatedType->isPointerTy()) {
                            pointeeType = getPointerElementTypeSafely(dyn_cast<PointerType>(allocatedType), varName);
                        } else {
                            pointeeType = allocatedType;
                        }
                        
                        charPtr = builder.CreateInBoundsGEP(
                            pointeeType, strPtr, idxVal, "char_ptr");
                    }
                    
                    Value* charVal = builder.CreateLoad(Type::getInt8Ty(context), charPtr, "char");
                    
                    VIX_DEBUG_LOG << "[DEBUG] string index: " << varName << "[i] = char (i8)\n";
                    
                    return VisitResult(charVal, ValueType::INT8);
                }
            }
        }
        AllocaInst* baseAlloc = nullptr;
        std::string varName;
        if (target->type == AST_IDENTIFIER) {
            varName = std::string(target->data.identifier.name);
            baseAlloc = scopeManager.findVariable(varName);
            if (!baseAlloc) baseAlloc = findVariableInMain(varName);
        }

        VisitResult idxRes = visit(indexNode);
        if (!idxRes.value) return VisitResult();
        Value* idxVal = idxRes.value;
        if (!idxVal->getType()->isIntegerTy(32)) {
            idxVal = builder.CreateIntCast(idxVal, Type::getInt32Ty(context), true, "idxcast");
        }

        if (baseAlloc) {
            Type* allocatedType = getActualType(baseAlloc);
            if (allocatedType && allocatedType->isArrayTy()) {
                ArrayType* at = cast<ArrayType>(allocatedType);
                Type* elemType = at->getElementType();
                Value* gep = builder.CreateInBoundsGEP(allocatedType, baseAlloc, 
                    {ConstantInt::get(Type::getInt32Ty(context),0), idxVal}, "arr_index_ptr");
                Value* loaded = builder.CreateLoad(elemType, gep, "arr_index_load");
                ValueType vt = typeHelper.getValueTypeFromType(elemType);
                return VisitResult(loaded, vt);
            }
            
            if (allocatedType && allocatedType->isPointerTy()) {
                Value* arrayPtr = builder.CreateLoad(allocatedType, baseAlloc, "array_ptr");
                Type* elemType = getPointerElementTypeSafely(dyn_cast<PointerType>(allocatedType), varName);
                if (varName == "argv") {//处理argv特殊情况
                    elemType = PointerType::getUnqual(Type::getInt8Ty(context));
                }
                ValueType vt = typeHelper.getValueTypeFromType(elemType);

                Function* fn = builder.GetInsertBlock() ? builder.GetInsertBlock()->getParent() : nullptr;
                if (!fn) return VisitResult();

                BasicBlock* nullBB = BasicBlock::Create(context, "idx_null", fn);
                BasicBlock* loadBB = BasicBlock::Create(context, "idx_load", fn);
                BasicBlock* contBB = BasicBlock::Create(context, "idx_cont", fn);

                Value* isNull = builder.CreateIsNull(arrayPtr, "idx_ptr_is_null");
                builder.CreateCondBr(isNull, nullBB, loadBB);

                builder.SetInsertPoint(nullBB);
                Value* nullVal = Constant::getNullValue(elemType);
                builder.CreateBr(contBB);

                builder.SetInsertPoint(loadBB);
                Value* gep = builder.CreateInBoundsGEP(elemType, arrayPtr, idxVal, "ptr_index_ptr");
                Value* loaded = builder.CreateLoad(elemType, gep, "ptr_index_load");
                builder.CreateBr(contBB);

                builder.SetInsertPoint(contBB);
                PHINode* result = builder.CreatePHI(elemType, 2, "idx_safe_val");
                result->addIncoming(nullVal, nullBB);
                result->addIncoming(loaded, loadBB);
                if (elemType->isPointerTy()) {
                    pointerElementHints[result] = Type::getInt32Ty(context);
                }
                return VisitResult(result, vt);
            }
        }
        VisitResult targetRes = visit(target);
        if (!targetRes.value) return VisitResult();

        if (AllocaInst* alloc = dyn_cast<AllocaInst>(targetRes.value)) {
            Type* allocatedType = getActualType(alloc);
            if (allocatedType && allocatedType->isArrayTy()) {
                ArrayType* at = cast<ArrayType>(allocatedType);
                Type* elemType = at->getElementType();
                Value* gep = builder.CreateInBoundsGEP(allocatedType, alloc, 
                    {ConstantInt::get(Type::getInt32Ty(context),0), idxVal}, "arr_index_ptr2");
                Value* loaded = builder.CreateLoad(elemType, gep, "arr_index_load2");
                ValueType vt = typeHelper.getValueTypeFromType(elemType);
                return VisitResult(loaded, vt);
            }
        }

        if (targetRes.value->getType()->isPointerTy()) {
            if (target->type == AST_IDENTIFIER) {
                varName = std::string(target->data.identifier.name);
            }
            Type* elemType = nullptr;
            auto hintIt = pointerElementHints.find(targetRes.value);
            if (hintIt != pointerElementHints.end() && hintIt->second) {
                elemType = hintIt->second;
            }
            if (!elemType) {
                elemType = getPointerElementTypeSafely(dyn_cast<PointerType>(targetRes.value->getType()), varName);
            }
            if (!elemType) {
                elemType = Type::getInt8Ty(context);
            }
            ValueType vt = typeHelper.getValueTypeFromType(elemType);

            Function* fn = builder.GetInsertBlock() ? builder.GetInsertBlock()->getParent() : nullptr;
            if (!fn) return VisitResult();

            BasicBlock* nullBB = BasicBlock::Create(context, "idx_null2", fn);
            BasicBlock* loadBB = BasicBlock::Create(context, "idx_load2", fn);
            BasicBlock* contBB = BasicBlock::Create(context, "idx_cont2", fn);

            Value* isNull = builder.CreateIsNull(targetRes.value, "idx_ptr_is_null2");
            builder.CreateCondBr(isNull, nullBB, loadBB);

            builder.SetInsertPoint(nullBB);
            Value* nullVal = Constant::getNullValue(elemType);
            builder.CreateBr(contBB);

            builder.SetInsertPoint(loadBB);
            Value* gep = builder.CreateInBoundsGEP(elemType, targetRes.value, idxVal, "arr_index_ptr3");
            Value* loaded = builder.CreateLoad(elemType, gep, "arr_index_load3");
            builder.CreateBr(contBB);

            builder.SetInsertPoint(contBB);
            PHINode* result = builder.CreatePHI(elemType, 2, "idx_safe_val2");
            result->addIncoming(nullVal, nullBB);
            result->addIncoming(loaded, loadBB);
            if (elemType->isPointerTy()) {
                pointerElementHints[result] = Type::getInt32Ty(context);
            }
            return VisitResult(result, vt);
        }

        return VisitResult();
    }
};

// ==================== C API ====================
extern "C" void vix_optimize_module(void* llvm_module, int level);
static int g_vix_opt_level = 0;
extern "C" void vix_set_opt_level(int level) { g_vix_opt_level = level; }
extern "C" int vix_get_opt_level(void) { return g_vix_opt_level; }

void llvm_emit_from_ast(ASTNode* ast_root, FILE* llvm_fp) {
    if (!ast_root || !llvm_fp) return;
    
    LLVMCodeGenerator generator;
    std::unique_ptr<Module> module = generator.generate(ast_root);
    
    if (module) {
        if (g_vix_opt_level > 0) {
            vix_optimize_module(module.get(), g_vix_opt_level);
        }
        std::string llvm_ir;
        raw_string_ostream ros(llvm_ir);
        module->print(ros, nullptr);
        fprintf(llvm_fp, "%s", llvm_ir.c_str());
    }
}

extern "C" void llvm_set_target_triple(const char* triple) {
    if (triple && triple[0] != '\0') {
        g_vix_target_triple = triple;
    } else {
        g_vix_target_triple.clear();
    }
}//main.c接口