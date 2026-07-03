#include "Codegen.h"
#include "../../include/compat.h"

using namespace llvm;

LLVMCodeGenerator::LLVMCodeGenerator() : builder(context), typeHelper(context) {
    module = std::make_unique<Module>("VixModule", context);
    std::string Triple = g_vix_target_triple.empty() ? sys::getProcessTriple() : g_vix_target_triple;
    llvm::Triple targetTriple(Triple);
    module->setTargetTriple(targetTriple);
    printfFunction = nullptr;
    strlenFunction = nullptr;
    strcpyFunction = nullptr;
    strcatFunction = nullptr;
    isGlobalScope = true;
    mainFunctionCreated = false;
    sourceAttrs = parseSourceAttributes(current_input_filename);
    initTarget();
}

std::unique_ptr<Module> LLVMCodeGenerator::generate(ASTNode* ast_root) {
    if (!ast_root) return nullptr;

    initPrintf();
    initStrlen();
    visit(ast_root);

    bool hasMain = module->getFunction("main") != nullptr;

    if (!hasMain && !mainFunctionCreated && !sourceAttrs.noMain) {
        report_semantic_error_with_location(
            "No 'fn main()' defined. " VIX_VERSION_STRING " requires an explicit main function.",
            current_input_filename ? current_input_filename : "unknown", 1);
    }
    Function* mainFunc = module->getFunction("main");
    if (mainFunc) {
        BasicBlock* endBB = nullptr;
        for (auto& BB : *mainFunc) {
            if (BB.getName() == "func_end") { endBB = &BB; break; }
        }
        if (!endBB) endBB = BasicBlock::Create(context, "func_end", mainFunc);
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
        return nullptr;
    }
    return std::move(module);
}

void LLVMCodeGenerator::initPrintf() {
    if (printfFunction) return;
    std::vector<Type*> printfArgs;
    printfArgs.push_back(PointerType::get(context, 0));
    FunctionType* printfType = FunctionType::get(Type::getInt32Ty(context), printfArgs, true);
    printfFunction = Function::Create(printfType, Function::ExternalLinkage, "printf", module.get());
    printfFunction->setCallingConv(CallingConv::C);
}

void LLVMCodeGenerator::createDefaultMain() {
    if (module->getFunction("main") || mainFunctionCreated) return;
    std::vector<Type*> mainParamTypes;
    mainParamTypes.push_back(Type::getInt32Ty(context));
    mainParamTypes.push_back(PointerType::get(context, 0));
    FunctionType* mainType = FunctionType::get(Type::getInt32Ty(context), mainParamTypes, false);
    Function* mainFunc = Function::Create(mainType, Function::ExternalLinkage, "main", module.get());
    mainFunctionCreated = true;
    BasicBlock* entryBB = BasicBlock::Create(context, "entry", mainFunc);
    builder.SetInsertPoint(entryBB);
    auto arg_it = mainFunc->arg_begin();
    arg_it->setName("argc");
    (++arg_it)->setName("argv");
    scopeManager.setCurrentFunction(mainFunc);
}

Function* LLVMCodeGenerator::getCurrentFunction() {
    return scopeManager.getCurrentFunction();
}

void LLVMCodeGenerator::reportCodegenSemanticError(ASTNode* node, const std::string& message) {
    const char* filename = (node && node->source_file) ? node->source_file :
        (current_input_filename ? current_input_filename : "unknown");
    int line = (node && node->location.first_line > 0) ? node->location.first_line : 1;
    report_semantic_error_with_location(message.c_str(), filename, line);
}

bool LLVMCodeGenerator::usesStructSRet(Function* func) const {
    if (!func) return false;
    return functionSRetResultTypesByFunc.find(func) != functionSRetResultTypesByFunc.end();
}

StructType* LLVMCodeGenerator::getStructSRetType(Function* func) const {
    if (!func) return nullptr;
    auto it = functionSRetResultTypesByFunc.find(func);
    return it != functionSRetResultTypesByFunc.end() ? it->second : nullptr;
}

void LLVMCodeGenerator::registerStructSRetFunction(const std::string& funcName, Function* func, StructType* structType) {
    if (!func || !structType) return;
    functionSRetResultTypesByName[funcName] = structType;
    functionSRetResultTypesByFunc[func] = structType;
}

std::string LLVMCodeGenerator::sanitizeTypeToken(const std::string& raw) {
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
    if (out.empty()) return "unk";
    return out;
}

std::string LLVMCodeGenerator::typeNodeToMangleToken(ASTNode* typeNode) const {
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
        default: return "unk";
    }
}

std::string LLVMCodeGenerator::mangleGenericFunctionName(const std::string& baseName, ASTNode* typeArgs) const {
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

bool LLVMCodeGenerator::bindGenericTypeArgs(ASTNode* fnNode, ASTNode* typeArgs, std::map<std::string, Type*>& outBindings) {
    outBindings.clear();
    if (!fnNode || fnNode->type != AST_FUNCTION) return false;
    ASTNode* genericParams = fnNode->data.function.generic_params;
    if (!genericParams || genericParams->type != AST_EXPRESSION_LIST) return false;
    if (!typeArgs) {
        int paramCount = genericParams->data.expression_list.expression_count;
        for (int i = 0; i < paramCount; i++) {
            ASTNode* p = genericParams->data.expression_list.expressions[i];
            if (!p || p->type != AST_IDENTIFIER || !p->data.identifier.name) return false;
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
        if (!p || p->type != AST_IDENTIFIER || !p->data.identifier.name || !a) return false;
        Type* concrete = typeHelper.getTypeFromTypeNode(a);
        outBindings[p->data.identifier.name] = concrete;
    }
    return true;
}

Function* LLVMCodeGenerator::instantiateGenericFunction(const std::string& baseName, ASTNode* typeArgs) {
    auto fit = genericFunctionTemplates.find(baseName);
    if (fit == genericFunctionTemplates.end()) return nullptr;

    std::string mangledName = mangleGenericFunctionName(baseName, typeArgs);
    if (Function* existing = module->getFunction(mangledName)) return existing;

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

    if (savedIP.isSet()) builder.restoreIP(savedIP);

    activeGenericTypeBindings = oldBindings;
    typeHelper.setGenericTypeBindings(activeGenericTypeBindings);
    if (!res.value) return nullptr;
    return module->getFunction(mangledName);
}

bool LLVMCodeGenerator::ensureValidInsertPoint() {
    BasicBlock* currentBB = builder.GetInsertBlock();
    if (currentBB) return true;
    Function* mainFunc = module->getFunction("main");
    if (!mainFunc) return false;
    BasicBlock* lastBB = nullptr;
    for (auto& bb : *mainFunc) lastBB = &bb;
    if (lastBB && !lastBB->getTerminator()) {
        builder.SetInsertPoint(lastBB);
        return true;
    }
    BasicBlock* newBB = BasicBlock::Create(context, "block", mainFunc);
    builder.SetInsertPoint(newBB);
    return true;
}

Value* LLVMCodeGenerator::safeCreateGlobalString(const std::string& str, const std::string& name) {
    if (!ensureValidInsertPoint()) return nullptr;
    if (str.empty()) return builder.CreateGlobalString("", name);
    return builder.CreateGlobalString(str, name);
}

Type* LLVMCodeGenerator::getActualType(AllocaInst* alloc) {
    if (!alloc) return nullptr;
    return alloc->getAllocatedType();
}

Type* LLVMCodeGenerator::getLLVMTypeFromTypeInfo(const TypeInfo* info) {
    if (!info) return nullptr;
    switch (info->kind) {
        case TYPEINFO_VOID: return Type::getVoidTy(context);
        case TYPEINFO_I8: return Type::getInt8Ty(context);
        case TYPEINFO_I32: return Type::getInt32Ty(context);
        case TYPEINFO_I64: return Type::getInt64Ty(context);
        case TYPEINFO_F32: return Type::getFloatTy(context);
        case TYPEINFO_F64: return Type::getDoubleTy(context);
        case TYPEINFO_BOOL: return Type::getInt1Ty(context);
        case TYPEINFO_STRING: return PointerType::get(context, 0);
        case TYPEINFO_PTR: {
            Type* elem = getLLVMTypeFromTypeInfo(info->element);
            if (!elem) elem = Type::getInt8Ty(context);
            return PointerType::get(context, 0);
        }
        case TYPEINFO_STRUCT: {
            if (info->name) {
                StructType* st = typeHelper.getStructType(info->name);
                if (st) return st;
            }
            return PointerType::get(context, 0);
        }
        case TYPEINFO_ARRAY: {
            Type* elem = getLLVMTypeFromTypeInfo(info->element);
            if (!elem) elem = Type::getInt8Ty(context);
            return PointerType::get(context, 0);
        }
        case TYPEINFO_FIXED_ARRAY: {
            Type* elem = getLLVMTypeFromTypeInfo(info->element);
            if (!elem) elem = Type::getInt8Ty(context);
            return ArrayType::get(elem, info->size);
        }
        case TYPEINFO_FN: return PointerType::get(context, 0);
        case TYPEINFO_APP: {
            if (info->app_ctor && info->app_ctor->kind == TYPEINFO_STRUCT && info->app_ctor->name) {
                StructType* st = typeHelper.getStructType(info->app_ctor->name);
                if (st) return st;
            }
            return PointerType::get(context, 0);
        }
        case TYPEINFO_VAR:
        default: return PointerType::get(context, 0);
    }
}

ValueType LLVMCodeGenerator::getValueTypeFromTypeInfo(const TypeInfo* info) {
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
        case TYPEINFO_FIXED_ARRAY: return ValueType::ARRAY;
        case TYPEINFO_PTR:
        case TYPEINFO_STRUCT:
        case TYPEINFO_FN:
        case TYPEINFO_APP:
        case TYPEINFO_VAR:
        default: return ValueType::POINTER;
    }
}

Type* LLVMCodeGenerator::getInferredLLVMType(ASTNode* node) {
    if (!node || !node->inferred_type) return nullptr;
    return getLLVMTypeFromTypeInfo(node->inferred_type);
}

Type* LLVMCodeGenerator::getInferredArrayElementType(ASTNode* node) {
    if (!node || !node->inferred_type) return nullptr;
    const TypeInfo* info = node->inferred_type;
    if (info->kind == TYPEINFO_ARRAY || info->kind == TYPEINFO_FIXED_ARRAY)
        return getLLVMTypeFromTypeInfo(info->element);
    return nullptr;
}

Type* LLVMCodeGenerator::getInferredPointerElementType(ASTNode* node) {
    if (!node || !node->inferred_type) return nullptr;
    if (node->inferred_type->kind == TYPEINFO_PTR)
        return getLLVMTypeFromTypeInfo(node->inferred_type->element);
    if (node->inferred_type->kind == TYPEINFO_ARRAY || node->inferred_type->kind == TYPEINFO_FIXED_ARRAY)
        return getLLVMTypeFromTypeInfo(node->inferred_type->element);
    return nullptr;
}

AllocaInst* LLVMCodeGenerator::findVariableInMain(const std::string& name) {
    Function* mainFunc = module->getFunction("main");
    if (!mainFunc) return nullptr;
    BasicBlock& entryBlock = mainFunc->getEntryBlock();
    for (Instruction& inst : entryBlock) {
        if (AllocaInst* alloc = dyn_cast<AllocaInst>(&inst)) {
            if (alloc->hasName() && alloc->getName() == name) return alloc;
        }
    }
    return nullptr;
}

GlobalVariable* LLVMCodeGenerator::findGlobalVariable(const std::string& name) {
    return module->getGlobalVariable(name);
}

void LLVMCodeGenerator::initStrlen() {
    if (strlenFunction) return;
    FunctionType* strlenType = FunctionType::get(
        Type::getInt64Ty(context), {PointerType::get(context, 0)}, false);
    strlenFunction = Function::Create(strlenType, Function::ExternalLinkage, "strlen", module.get());
    strlenFunction->setCallingConv(CallingConv::C);
}

Function* LLVMCodeGenerator::getOrCreateStrcpyFunction() {
    if (strcpyFunction) return strcpyFunction;
    Type* i8PtrTy = PointerType::get(context, 0);
    FunctionType* strcpyType = FunctionType::get(i8PtrTy, {i8PtrTy, i8PtrTy}, false);
    strcpyFunction = Function::Create(strcpyType, Function::ExternalLinkage, "strcpy", module.get());
    strcpyFunction->setCallingConv(CallingConv::C);
    return strcpyFunction;
}

Function* LLVMCodeGenerator::getOrCreateStrcatFunction() {
    if (strcatFunction) return strcatFunction;
    Type* i8PtrTy = PointerType::get(context, 0);
    FunctionType* strcatType = FunctionType::get(i8PtrTy, {i8PtrTy, i8PtrTy}, false);
    strcatFunction = Function::Create(strcatType, Function::ExternalLinkage, "strcat", module.get());
    strcatFunction->setCallingConv(CallingConv::C);
    return strcatFunction;
}

Function* LLVMCodeGenerator::getOrCreateReallocFunction() {
    if (Function* fn = module->getFunction("realloc")) return fn;
    Type* i8PtrTy = PointerType::get(context, 0);
    Type* i64Ty = Type::getInt64Ty(context);
    FunctionType* reallocType = FunctionType::get(i8PtrTy, {i8PtrTy, i64Ty}, false);
    Function* reallocFn = Function::Create(reallocType, Function::ExternalLinkage, "realloc", module.get());
    reallocFn->setCallingConv(CallingConv::C);
    return reallocFn;
}

Type* LLVMCodeGenerator::getPointerElementTypeSafely(PointerType* ptrType, const std::string& varName) {
    if (!ptrType) return Type::getInt8Ty(context);
    if (varName.empty()) return Type::getInt8Ty(context);

    if (typeHelper.isStringVariable(varName)) {
        VIX_DEBUG_LOG << "[DEBUG] String variable '" << varName << "': using i8 as element type\n";
        return Type::getInt8Ty(context);
    }
    if (varName == "argv") {
        VIX_DEBUG_LOG << "[DEBUG] argv: using char** as element type\n";
        return PointerType::get(context, 0);
    }
    AllocaInst* alloc = scopeManager.findVariable(varName);
    if (!alloc) alloc = findVariableInMain(varName);
    if (alloc) {
        Type* allocatedType = getActualType(alloc);
        if (allocatedType && allocatedType->isArrayTy()) {
            ArrayType* arrType = cast<ArrayType>(allocatedType);
            Type* elemType = arrType->getElementType();
            VIX_DEBUG_LOG << "[DEBUG] Local array '" << varName << "': using element type from alloc: " << *elemType << "\n";
            return elemType;
        }
    }
    if (auto* arrayInfo = typeHelper.getArrayTypeInfo(varName)) {
        VIX_DEBUG_LOG << "[DEBUG] Array variable '" << varName << "': using element type from array info: " << *arrayInfo->first << "\n";
        return arrayInfo->first;
    }
    if (GlobalVariable* gv = module->getGlobalVariable(varName)) {
        Type* globalType = gv->getValueType();
        if (globalType->isArrayTy()) {
            ArrayType* arrType = cast<ArrayType>(globalType);
            Type* elemType = arrType->getElementType();
            VIX_DEBUG_LOG << "[DEBUG] Global array '" << varName << "': using element type from global: " << *elemType << "\n";
            return elemType;
        }
    }
    if (varName.find("str") != std::string::npos || varName.find("Str") != std::string::npos ||
        varName.find("STRING") != std::string::npos || varName.find("lit") != std::string::npos) {
        VIX_DEBUG_LOG << "[DEBUG] Variable '" << varName << "' looks like a string: using i8\n";
        return Type::getInt8Ty(context);
    }
    VIX_DEBUG_LOG << "[DEBUG] Unknown pointer '" << varName << "': defaulting to i8\n";
    return Type::getInt8Ty(context);
}

bool LLVMCodeGenerator::isArrayParamPosition(const std::string& funcName, int userParamIndex) {
    auto it = functionArrayParamPositions.find(funcName);
    if (it == functionArrayParamPositions.end()) return false;
    for (int pos : it->second) {
        if (pos == userParamIndex) return true;
    }
    return false;
}

Value* LLVMCodeGenerator::getRuntimeArrayLengthValue(const std::string& varName) {
    std::string lenVarName = varName + "__len";
    AllocaInst* lenAlloc = scopeManager.findVariable(lenVarName);
    if (!lenAlloc) lenAlloc = findVariableInMain(lenVarName);
    if (!lenAlloc) return nullptr;
    Type* lenType = getActualType(lenAlloc);
    if (!lenType || !lenType->isIntegerTy()) return nullptr;
    Value* lenVal = builder.CreateLoad(lenType, lenAlloc, lenVarName + "_val");
    if (!lenVal->getType()->isIntegerTy(32))
        lenVal = builder.CreateIntCast(lenVal, Type::getInt32Ty(context), false, "arr_len_cast");
    return lenVal;
}

Value* LLVMCodeGenerator::inferArrayLengthFromArgument(ASTNode* argNode) {
    if (argNode && argNode->type == AST_IDENTIFIER && argNode->data.identifier.name) {
        std::string varName(argNode->data.identifier.name);
        if (Value* runtimeLen = getRuntimeArrayLengthValue(varName)) return runtimeLen;
        AllocaInst* alloc = scopeManager.findVariable(varName);
        if (!alloc) alloc = findVariableInMain(varName);
        if (alloc) {
            Type* allocatedType = getActualType(alloc);
            if (allocatedType && allocatedType->isArrayTy()) {
                uint64_t numElements = cast<ArrayType>(allocatedType)->getNumElements();
                return ConstantInt::get(Type::getInt32Ty(context), numElements);
            }
            if (allocatedType && allocatedType->isPointerTy()) {
                Value* dataPtr = builder.CreateLoad(allocatedType, alloc, varName + "_arr_ptr");
                return emitLoadArrayLength(dataPtr, varName + "_arg_len");
            }
        }
        if (GlobalVariable* global = findGlobalVariable(varName)) {
            Type* globalType = global->getValueType();
            if (globalType && globalType->isPointerTy()) {
                Value* dataPtr = builder.CreateLoad(globalType, global, varName + "_arr_ptr");
                return emitLoadArrayLength(dataPtr, varName + "_arg_len");
            }
        }
        if (auto* arrayInfo = typeHelper.getArrayTypeInfo(varName)) {
            if (arrayInfo->second >= 0)
                return ConstantInt::get(Type::getInt32Ty(context), arrayInfo->second);
        }
        int knownSize = typeHelper.getVariableArraySize(varName);
        if (knownSize >= 0)
            return ConstantInt::get(Type::getInt32Ty(context), knownSize);
    }
    if (argNode && argNode->type == AST_EXPRESSION_LIST) {
        int count = argNode->data.expression_list.expression_count;
        return ConstantInt::get(Type::getInt32Ty(context), count);
    }
    if (argNode && argNode->type == AST_MEMBER_ACCESS) {
        VisitResult memberRes = visit(argNode);
        if (memberRes.value && memberRes.value->getType()->isPointerTy()) {
            return emitLoadArrayLength(memberRes.value, "member_arg_len");
        }
    }
    if (argNode && argNode->type == AST_INDEX) {
        VisitResult indexRes = visit(argNode);
        if (indexRes.value && indexRes.value->getType()->isPointerTy()) {
            return emitLoadArrayLength(indexRes.value, "index_arg_len");
        }
    }
    return ConstantInt::get(Type::getInt32Ty(context), 0);
}

AllocaInst* LLVMCodeGenerator::findRuntimeArrayLengthSlot(const std::string& varName) {
    std::string lenVarName = varName + "__len";
    AllocaInst* lenAlloc = scopeManager.findVariable(lenVarName);
    if (!lenAlloc) lenAlloc = findVariableInMain(lenVarName);
    return lenAlloc;
}

AllocaInst* LLVMCodeGenerator::ensureRuntimeArrayLengthSlot(const std::string& varName, int initialLen) {
    AllocaInst* existing = findRuntimeArrayLengthSlot(varName);
    if (existing) return existing;
    Function* func = getCurrentFunction();
    if (!func) {
        func = module->getFunction("main");
        if (!func) { createDefaultMain(); func = module->getFunction("main"); }
    }
    if (!func) return nullptr;
    BasicBlock* entryBB = &func->getEntryBlock();
    BasicBlock* savedBB = builder.GetInsertBlock();
    std::string lenVarName = varName + "__len";
    IRBuilder<> tempBuilder(entryBB, entryBB->begin());
    AllocaInst* lenAlloc = tempBuilder.CreateAlloca(Type::getInt32Ty(context), nullptr, lenVarName);
    tempBuilder.CreateStore(ConstantInt::get(Type::getInt32Ty(context), initialLen), lenAlloc);
    if (savedBB) builder.SetInsertPoint(savedBB);
    scopeManager.defineVariable(lenVarName, lenAlloc);
    return lenAlloc;
}

Value* LLVMCodeGenerator::emitStringConcat(Value* left, Value* right) {
    if (!left || !right) return nullptr;
    Type* i8PtrTy = PointerType::get(context, 0);
    if (left->getType() != i8PtrTy)
        left = builder.CreateBitCast(left, i8PtrTy, "strcat_left_cast");
    if (right->getType() != i8PtrTy)
        right = builder.CreateBitCast(right, i8PtrTy, "strcat_right_cast");

    Value* empty = safeCreateGlobalString("", "strcat_empty");
    if (empty) {
        left = builder.CreateSelect(builder.CreateIsNull(left, "strcat_left_is_null"), empty, left, "strcat_left_safe");
        right = builder.CreateSelect(builder.CreateIsNull(right, "strcat_right_is_null"), empty, right, "strcat_right_safe");
    }

    initStrlen();
    Function* reallocFn = getOrCreateReallocFunction();
    Function* strcpyFn = getOrCreateStrcpyFunction();
    Function* strcatFn = getOrCreateStrcatFunction();

    Value* leftLen = builder.CreateCall(strlenFunction, {left}, "strcat_lhs_len");
    Value* rightLen = builder.CreateCall(strlenFunction, {right}, "strcat_rhs_len");
    Value* totalLen = builder.CreateAdd(leftLen, rightLen, "strcat_total_len");
    Value* allocSize = builder.CreateAdd(totalLen, ConstantInt::get(Type::getInt64Ty(context), 1), "strcat_alloc_sz");
    Value* buf = builder.CreateCall(reallocFn, {ConstantPointerNull::get(PointerType::get(context, 0)), allocSize}, "strcat_buf");
    builder.CreateCall(strcpyFn, {buf, left});
    builder.CreateCall(strcatFn, {buf, right});
    return buf;
}

Value* LLVMCodeGenerator::emitLoadArrayLength(Value* dataPtr, const Twine& name) {
    if (!dataPtr || !dataPtr->getType()->isPointerTy())
        return ConstantInt::get(Type::getInt32Ty(context), 0);

    PointerType* i8PtrTy = PointerType::get(context, 0);
    Value* dataI8 = builder.CreateBitCast(dataPtr, i8PtrTy, name + "_as_i8");

    Function* func = builder.GetInsertBlock()->getParent();
    BasicBlock* nullBB = BasicBlock::Create(context, name + "_null", func);
    BasicBlock* nonNullBB = BasicBlock::Create(context, name + "_nonnull", func);
    BasicBlock* mergeBB = BasicBlock::Create(context, name + "_merge", func);

    Value* isNull = builder.CreateIsNull(dataI8, name + "_is_null");
    builder.CreateCondBr(isNull, nullBB, nonNullBB);

    builder.SetInsertPoint(nonNullBB);
    Value* ptrInt = builder.CreatePtrToInt(dataI8, Type::getInt64Ty(context));
    Value* headerInt = builder.CreateSub(ptrInt, ConstantInt::get(Type::getInt64Ty(context), ARRAY_HEADER_BYTES));
    Value* headerPtr = builder.CreateIntToPtr(headerInt, i8PtrTy);
    Value* lenPtr = builder.CreateBitCast(headerPtr, PointerType::get(context, 0));
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

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::emitFunctionPointerCall(Value* rawCalleePtr, ASTNode* argsNode, Type* expectedReturnTypeHint) {
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
        if (firstArgType->isIntegerTy() || firstArgType->isPointerTy() || firstArgType->isFloatingPointTy())
            returnType = firstArgType;
    }

    FunctionType* fnType = FunctionType::get(returnType, argTypes, false);
    if ((int)fnType->getNumParams() != (int)argValues.size()) return VisitResult();

    for (size_t i = 0; i < argValues.size(); i++) {
        Type* expectedArgTy = fnType->getParamType((unsigned)i);
        Value* argVal = argValues[i];
        if (argVal->getType() != expectedArgTy) {
            ValueType toVT = typeHelper.getValueTypeFromType(expectedArgTy);
            ValueType fromVT = typeHelper.getValueTypeFromType(argVal->getType());
            argVal = typeHelper.castValue(builder, argVal, fromVT, toVT);
            if (argVal->getType() != expectedArgTy && argVal->getType()->isPointerTy() && expectedArgTy->isPointerTy())
                argVal = builder.CreateBitCast(argVal, expectedArgTy, "fparg_ptrcast");
            argValues[i] = argVal;
        }
    }

    Value* typedFnPtr = builder.CreateBitCast(rawCalleePtr, PointerType::get(context, 0), "fnptr_cast");
    CallInst* callInst = builder.CreateCall(fnType, typedFnPtr, argValues, "fpcalltmp");
    return VisitResult(callInst, typeHelper.getValueTypeFromType(returnType));
}

Value* LLVMCodeGenerator::getBuiltinUnionCtorTagValue(const std::string& ctorName) {
    GlobalVariable* ctorGlobal = module->getGlobalVariable(ctorName, true);
    if (ctorGlobal && ctorGlobal->hasInitializer()) {
        if (auto* intInit = dyn_cast<ConstantInt>(ctorGlobal->getInitializer()))
            return ConstantInt::get(Type::getInt32Ty(context), intInit->getSExtValue(), true);
    }
    int32_t fallbackTag = 0;
    if (ctorName == "Some") fallbackTag = 1;
    if (ctorName == "Err") fallbackTag = 1;
    return ConstantInt::get(Type::getInt32Ty(context), fallbackTag, true);
}

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visit(ASTNode* node) {
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

Constant* LLVMCodeGenerator::evaluateConstExpr(ASTNode* node, ValueType* outType) {
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
            return ConstantInt::get(Type::getInt64Ty(context), static_cast<uint64_t>(val), true);
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
            if (node->data.unaryop.op == OP_NOT) {
                if (auto* ci = dyn_cast<ConstantInt>(rhs)) {
                    bool val = ci->getZExtValue() != 0;
                    return ConstantInt::get(Type::getInt1Ty(context), val ? 0 : 1);
                }
                return nullptr;
            }
            if (node->data.unaryop.op == OP_MINUS) {
                if (auto* ci = dyn_cast<ConstantInt>(rhs)) {
                    int64_t value = ci->getSExtValue();
                    int bits = ci->getType()->isIntegerTy(64) ? 64 : 32;
                    if (outType) *outType = (bits == 64) ? ValueType::INT64 : ValueType::INT32;
                    if (bits == 64)
                        return ConstantInt::get(Type::getInt64Ty(context), static_cast<uint64_t>(-value), true);
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
                case OP_POW: {
                    if (rv < 0) return nullptr;
                    int64_t base = lv;
                    int64_t exp = rv;
                    int64_t acc = 1;
                    while (exp > 0) {
                        if (exp & 1) acc *= base;
                        base *= base;
                        exp >>= 1;
                    }
                    result = acc;
                    break;
                }
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
            if (!typeNameNode || typeNameNode->type != AST_IDENTIFIER || !typeNameNode->data.identifier.name)
                return nullptr;

            std::string structName(typeNameNode->data.identifier.name);
            StructType* st = typeHelper.getStructType(structName);
            auto* fields = typeHelper.getStructFields(structName);
            if (!st || !fields) return nullptr;

            std::vector<Constant*> values(fields->size(), nullptr);
            for (size_t i = 0; i < fields->size(); ++i)
                values[i] = Constant::getNullValue((*fields)[i].second);

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

// ==================== C API ====================
extern "C" void vix_optimize_module(void* llvm_module, int level);
static int g_vix_opt_level = 0;
extern "C" void vix_set_opt_level(int level) { g_vix_opt_level = level; }
extern "C" int vix_get_opt_level(void) { return g_vix_opt_level; }

extern "C" void llvm_emit_from_ast(ASTNode* ast_root, FILE* llvm_fp) {
    if (!ast_root || !llvm_fp) return;
    LLVMCodeGenerator generator;
    std::unique_ptr<Module> module = generator.generate(ast_root);
    if (module) {
        if (g_vix_opt_level > 0) vix_optimize_module(module.get(), g_vix_opt_level);
        std::string llvm_ir;
        raw_string_ostream ros(llvm_ir);
        module->print(ros, nullptr);
        fprintf(llvm_fp, "%s", llvm_ir.c_str());
    }
}

extern "C" void llvm_set_target_triple(const char* triple) {
    if (triple && triple[0] != '\0') g_vix_target_triple = triple;
    else g_vix_target_triple.clear();
}
