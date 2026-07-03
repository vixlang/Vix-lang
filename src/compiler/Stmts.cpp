#include "Codegen.h"

using namespace llvm;

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitProgram(ASTNode* node) {
    if (!node) return VisitResult();

    for (int i = 0; i < node->data.program.statement_count; i++) {
        ASTNode* stmt = node->data.program.statements[i];
        if (!stmt || stmt->type != AST_FUNCTION) continue;
        std::string name(stmt->data.function.name ? stmt->data.function.name : "");
        if (!name.empty()) {
            allFunctionNodes[name] = stmt;
        }
        ASTNode* gparams = stmt->data.function.generic_params;
        if (gparams && gparams->type == AST_EXPRESSION_LIST && gparams->data.expression_list.expression_count > 0) {
            if (!name.empty()) {
                genericFunctionTemplates[name] = stmt;
                genericFunctionArity[name] = gparams->data.expression_list.expression_count;
            }
        }
    }

    VisitResult lastResult;
    for (int i = 0; i < node->data.program.statement_count; i++) {
        ASTNode* stmt = node->data.program.statements[i];
        if (stmt && stmt->type == AST_FUNCTION) {
            ASTNode* gparams = stmt->data.function.generic_params;
            if (gparams && gparams->type == AST_EXPRESSION_LIST && gparams->data.expression_list.expression_count > 0) {
                continue;
            }
        }
        lastResult = visit(node->data.program.statements[i]);
        BasicBlock* currentBB = builder.GetInsertBlock();
        if (currentBB && currentBB->getTerminator()) {
            break;
        }
    }
    
    return lastResult;
}

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitBreak(ASTNode* node) {
    (void)node;
    if (loopBreakTargets.empty()) {
        llvm::errs() << "Error: 'break' used outside of loop\n";
        return VisitResult();
    }

    BasicBlock* target = loopBreakTargets.back();
    builder.CreateBr(target);
    return VisitResult();
}

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitContinue(ASTNode* node) {
    (void)node;
    if (loopContinueTargets.empty()) {
        llvm::errs() << "Error: 'continue' used outside of loop\n";
        return VisitResult();
    }

    BasicBlock* target = loopContinueTargets.back();
    builder.CreateBr(target);
    return VisitResult();
}

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitIf(ASTNode* node) {
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
    VisitResult thenResult = visit(node->data.if_stmt.then_body);
    scopeManager.exitScope();
    BasicBlock* thenEndBB = builder.GetInsertBlock();
    bool thenTerminated = !thenEndBB || thenEndBB->getTerminator();
    if (!thenTerminated) {
        builder.CreateBr(mergeBB);
    }
    
    func->insert(func->end(), elseBB);
    func->insert(func->end(), mergeBB);
    
    builder.SetInsertPoint(elseBB);
    VisitResult elseResult;
    if (node->data.if_stmt.else_body) {
        scopeManager.enterScope();
        elseResult = visit(node->data.if_stmt.else_body);
        scopeManager.exitScope();
    }
    BasicBlock* elseEndBB = builder.GetInsertBlock();
    bool elseTerminated = !elseEndBB || elseEndBB->getTerminator();
    if (!elseTerminated) {
        builder.CreateBr(mergeBB);
    }
    
    builder.SetInsertPoint(mergeBB);
    
    // Create phi node if both branches produce non-void values
    if (thenResult.value && elseResult.value && !thenTerminated && !elseTerminated) {
        ValueType resultType = thenResult.type;
        if (thenResult.type != elseResult.type) {
            auto [promotedLeft, promotedRight] = typeHelper.promoteTypes(thenResult.type, elseResult.type);
            resultType = (promotedLeft > promotedRight) ? promotedLeft : promotedRight;
            // Cast inside each branch block before the terminator so the casted
            // value dominates the merge block (LLVM requirement).
            if (thenResult.type != resultType && thenResult.value && thenEndBB->getTerminator()) {
                thenEndBB->getTerminator()->eraseFromParent();
                builder.SetInsertPoint(thenEndBB);
                thenResult.value = typeHelper.castValue(builder, thenResult.value, thenResult.type, resultType);
                builder.CreateBr(mergeBB);
                thenEndBB = builder.GetInsertBlock();
            }
            if (elseResult.type != resultType && elseResult.value && elseEndBB->getTerminator()) {
                elseEndBB->getTerminator()->eraseFromParent();
                builder.SetInsertPoint(elseEndBB);
                elseResult.value = typeHelper.castValue(builder, elseResult.value, elseResult.type, resultType);
                builder.CreateBr(mergeBB);
                elseEndBB = builder.GetInsertBlock();
            }
            // Restore insert point to mergeBB
            builder.SetInsertPoint(mergeBB);
        }
        if (thenResult.value && elseResult.value && thenResult.value->getType() == elseResult.value->getType() &&
            !thenResult.value->getType()->isVoidTy()) {
            PHINode* phi = builder.CreatePHI(thenResult.value->getType(), 2, "iftmp");
            phi->addIncoming(thenResult.value, thenEndBB);
            phi->addIncoming(elseResult.value, elseEndBB);
            return VisitResult(phi, resultType);
        }
    }
    
    return VisitResult();
}

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitWhile(ASTNode* node) {
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
    if (loopBB && !loopBB->getTerminator()) {
        builder.CreateBr(condBB);
    }
    
    builder.SetInsertPoint(afterBB);
    return VisitResult();
}

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitFor(ASTNode* node) {
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

        if (builder.GetInsertBlock() && !builder.GetInsertBlock()->getTerminator()) {
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
    Value* cond = builder.CreateICmpSLT(cur_val, end_val_casted, "forcond");
    VIX_DEBUG_LOG << "[DEBUG] for cond half-open ascending range\n";
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
    if (builder.GetInsertBlock() && !builder.GetInsertBlock()->getTerminator()) {
        builder.CreateBr(incBB);
    }
    builder.SetInsertPoint(incBB);
    Value* cur_val_for_inc = builder.CreateLoad(Type::getInt32Ty(context), var_alloc, var_name);
    Value* new_val = builder.CreateAdd(cur_val_for_inc, ConstantInt::get(Type::getInt32Ty(context), 1), "inc");
    builder.CreateStore(new_val, var_alloc);
    builder.CreateBr(condBB);
    
    builder.SetInsertPoint(afterBB);
    return VisitResult();
}

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitConst(ASTNode* node) {
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

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitGlobal(ASTNode* node) {
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

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitPrint(ASTNode* node) {
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
                Value* emptyStr = safeCreateGlobalString("", "empty_str");
                Value* formatStr = safeCreateGlobalString("%s", "fmt_s");
                builder.CreateCall(printfFunction, {formatStr, emptyStr});
                continue;
            }
            
            Value* printValue = exprRes.value;
            ValueType printType = exprRes.type;
            Value* formatStr = nullptr;
            
            switch (printType) {
                case ValueType::INT32:
                    if (expr->inferred_type && expr->inferred_type->kind == TYPEINFO_BOOL) {
                        Value* trueStr = safeCreateGlobalString("true", "bool_true");
                        Value* falseStr = safeCreateGlobalString("false", "bool_false");
                        Value* boolVal = builder.CreateICmpNE(printValue, ConstantInt::get(Type::getInt32Ty(context), 0), "to_bool");
                        Value* selected = builder.CreateSelect(boolVal, trueStr, falseStr, "bool_str");
                        formatStr = safeCreateGlobalString("%s", "fmt_bs");
                        builder.CreateCall(printfFunction, {formatStr, selected});
                    } else {
                        formatStr = safeCreateGlobalString("%d", "fmt_i32");
                        builder.CreateCall(printfFunction, {formatStr, printValue});
                    }
                    break;
                    
                case ValueType::INT8:
                    formatStr = safeCreateGlobalString("%c", "fmt_c");
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::INT64:
                    formatStr = safeCreateGlobalString("%lld", "fmt_i64");
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::FLOAT32:
                case ValueType::FLOAT64:
                    formatStr = safeCreateGlobalString("%f", "fmt_f");
                    printValue = typeHelper.castValue(builder, printValue, printType, ValueType::FLOAT64);
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::STRING:
                    formatStr = safeCreateGlobalString("%s", "fmt_s");
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::BOOL:
                    {
                        Value* trueStr = safeCreateGlobalString("true", "bool_true");
                        Value* falseStr = safeCreateGlobalString("false", "bool_false");
                        Value* boolVal = builder.CreateICmpNE(
                            typeHelper.castValue(builder, printValue, printType, ValueType::INT32),
                            ConstantInt::get(Type::getInt32Ty(context), 0), "to_bool");
                        Value* selected = builder.CreateSelect(boolVal, trueStr, falseStr, "bool_str");
                        formatStr = safeCreateGlobalString("%s", "fmt_b");
                        builder.CreateCall(printfFunction, {formatStr, selected});
                    }
                    break;
                    
                case ValueType::POINTER:
                    formatStr = safeCreateGlobalString("%p", "fmt_p");
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                case ValueType::ARRAY:
                    formatStr = safeCreateGlobalString("%p", "fmt_p");
                    builder.CreateCall(printfFunction, {formatStr, printValue});
                    break;
                    
                default:
                    formatStr = safeCreateGlobalString("%d", "fmt_d");
                    if (printValue->getType()->isIntegerTy()) {
                        builder.CreateCall(printfFunction, {formatStr, printValue});
                    } else {
                        Value* defaultVal = ConstantInt::get(Type::getInt32Ty(context), 0);
                        builder.CreateCall(printfFunction, {formatStr, defaultVal});
                    }
                    break;
            }
        }
        
        Value* newline = safeCreateGlobalString("\n", "fmt_nl");
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
                formatStr = safeCreateGlobalString("%d\n", "fmt_i32_nl");
                builder.CreateCall(printfFunction, {formatStr, printValue});
                break;
                
            case ValueType::INT8:
                formatStr = safeCreateGlobalString("%c\n", "fmt_c_nl");
                builder.CreateCall(printfFunction, {formatStr, printValue});
                break;
                
            case ValueType::INT64:
                formatStr = safeCreateGlobalString("%lld\n", "fmt_i64_nl");
                builder.CreateCall(printfFunction, {formatStr, printValue});
                break;
                
            case ValueType::FLOAT32:
            case ValueType::FLOAT64:
                formatStr = safeCreateGlobalString("%f\n", "fmt_f_nl");
                printValue = typeHelper.castValue(builder, printValue, printType, ValueType::FLOAT64);
                builder.CreateCall(printfFunction, {formatStr, printValue});
                break;
                
            case ValueType::STRING:
                formatStr = safeCreateGlobalString("%s\n", "fmt_s_nl");
                builder.CreateCall(printfFunction, {formatStr, printValue});
                break;
                
            case ValueType::BOOL:
                {
                    Value* trueStr = safeCreateGlobalString("true", "bool_true_nl");
                    Value* falseStr = safeCreateGlobalString("false", "bool_false_nl");
                    Value* boolVal = builder.CreateICmpNE(
                        typeHelper.castValue(builder, printValue, printType, ValueType::INT32),
                        ConstantInt::get(Type::getInt32Ty(context), 0), "to_bool");
                    Value* selected = builder.CreateSelect(boolVal, trueStr, falseStr, "bool_str");
                    formatStr = safeCreateGlobalString("%s\n", "fmt_b_nl");
                    builder.CreateCall(printfFunction, {formatStr, selected});
                }
                break;
                
            case ValueType::POINTER:
                formatStr = safeCreateGlobalString("%p\n", "fmt_p_nl");
                builder.CreateCall(printfFunction, {formatStr, printValue});
                break;
                
            case ValueType::ARRAY:
                formatStr = safeCreateGlobalString("%p\n", "fmt_p_nl");
                builder.CreateCall(printfFunction, {formatStr, printValue});
                break;
                
            default:
                formatStr = safeCreateGlobalString("%d\n", "fmt_d_nl");
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

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitInput(ASTNode* node) {
    (void)node;
    return VisitResult(ConstantInt::get(Type::getInt32Ty(context), 0), ValueType::INT32);
}

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitReturn(ASTNode* node) {
    Function* currentFunc = getCurrentFunction();
    if (!currentFunc) return VisitResult();
    
    Type* expectedReturnType = currentFunc->getReturnType();
    StructType* sretType = getStructSRetType(currentFunc);

    if (sretType) {
        Argument* sretArg = currentFunc->arg_begin();
        Value* sretPtr = sretArg;
        Type* expectSretPtrType = PointerType::get(context, 0);
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
                Type* expectSrcPtrType = PointerType::get(context, 0);
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
        
        /* Auto-deref: if returning a pointer but expected type is a value, load from the pointer */
        bool retIsPtr = (retVal.type == ValueType::POINTER || retVal.type == ValueType::STRING) && retVal.value->getType()->isPointerTy();
        if (retIsPtr && !expectedReturnType->isPointerTy() && !expectedReturnType->isVoidTy()) {
            Type* loadType = expectedReturnType;
            Value* loaded = builder.CreateLoad(loadType, retVal.value, "autoderef_ret");
            builder.CreateRet(loaded);
            return VisitResult(loaded, typeHelper.getValueTypeFromType(expectedReturnType));
        }
        
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

LLVMCodeGenerator::VisitResult LLVMCodeGenerator::visitAssign(ASTNode* node) {
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

        Type* elemType = nullptr;
        auto ptrHintIt = pointerElementHints.find(ptrRes.value);
        if (ptrHintIt != pointerElementHints.end() && ptrHintIt->second) {
            elemType = ptrHintIt->second;
        }
        if (!elemType && !ptrVarName.empty()) {
            AllocaInst* ptrAlloc = scopeManager.findVariable(ptrVarName);
            if (!ptrAlloc) ptrAlloc = findVariableInMain(ptrVarName);
            if (ptrAlloc) {
                auto allocHintIt = pointerElementHints.find(ptrAlloc);
                if (allocHintIt != pointerElementHints.end() && allocHintIt->second) {
                    elemType = allocHintIt->second;
                }
            }
        }
        if (!elemType) {
            elemType = getPointerElementTypeSafely(
                dyn_cast<PointerType>(ptrRes.value->getType()), ptrVarName);
        }
        if (!elemType) {
            elemType = Type::getInt32Ty(context);
        }

        Value* ptrVal = ptrRes.value;
        Type* expectPtrType = PointerType::get(context, 0);
        if (ptrVal->getType() != expectPtrType) {
            ptrVal = builder.CreateBitCast(ptrVal, expectPtrType, "deref_store_ptrcast");
        }

        if (elemType->isStructTy()) {
            VisitResult rightVal = visit(node->data.assign.right);
            if (!rightVal.value) return VisitResult();

            Value* structVal = nullptr;
            if (rightVal.value->getType() == elemType) {
                structVal = rightVal.value;
            } else if (rightVal.value->getType()->isPointerTy()) {
                Value* srcPtr = rightVal.value;
                if (srcPtr->getType() != expectPtrType) {
                    srcPtr = builder.CreateBitCast(srcPtr, expectPtrType, "deref_struct_src_cast");
                }
                structVal = builder.CreateLoad(elemType, srcPtr, "deref_struct_val");
            }

            if (structVal) {
                builder.CreateStore(structVal, ptrVal);
            }
            return VisitResult(ptrVal, ValueType::POINTER, cast<StructType>(elemType));
        }

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
        Type* expectPtrType = PointerType::get(context, 0);
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
            if (func) {
                Type* globalType = gvar->getValueType();
                ValueType gvt = typeHelper.getValueTypeFromType(globalType);
                Value* castedVal = typeHelper.castValue(builder, rightVal.value, rightVal.type, gvt);
                builder.CreateStore(castedVal, gvar);
                return VisitResult(castedVal, gvt);
            }
        }
        
        Function* func = getCurrentFunction();
        if (!func) {
            // Module-level declaration: create a GlobalVariable
            Type* globalType = rightVal.value->getType();
            if (isStringAssign) {
                globalType = PointerType::get(context, 0);
            }
            Constant* initVal = nullptr;
            if (isStringAssign && rightVal.value->getType()->isArrayTy()) {
                // String literal: create a global array and use GEP to get i8*
                GlobalVariable* strGV = new GlobalVariable(
                    *module, rightVal.value->getType(), true,
                    GlobalValue::PrivateLinkage,
                    cast<Constant>(rightVal.value), name + ".strdata");
                std::vector<Constant*> indices = {
                    ConstantInt::get(Type::getInt64Ty(context), 0),
                    ConstantInt::get(Type::getInt64Ty(context), 0)
                };
                initVal = ConstantExpr::getGetElementPtr(
                    rightVal.value->getType(), strGV, indices, true);
            } else if (isa<Constant>(rightVal.value)) {
                initVal = cast<Constant>(rightVal.value);
                if (initVal->getType() != globalType) {
                    if (initVal->getType()->isPointerTy() && globalType->isPointerTy()) {
                        // Both are pointers (opaque ptrs in LLVM 20) - no cast needed
                    } else {
                        initVal = ConstantExpr::getBitCast(initVal, globalType);
                    }
                }
            }
            if (!initVal) {
                initVal = Constant::getNullValue(globalType);
            }
            bool isConst = (node->data.assign.left->mutability != MUTABILITY_MUTABLE);
            GlobalVariable* gv = new GlobalVariable(
                *module, globalType, isConst,
                GlobalValue::ExternalLinkage, initVal, name);
            return VisitResult(gv, rightVal.type);
        }
        
        BasicBlock* entryBB = &func->getEntryBlock();
        BasicBlock* savedBB = builder.GetInsertBlock();
        
        Type* varType = nullptr;
        if (isStringAssign) {
            varType = PointerType::get(context, 0);
        } else {
            // Use inferred type from type checker if available, to get correct
            // type for ADT payloads (which are stored as ptr but should be i32, etc.)
            Type* inferredLLVM = nullptr;
            if (node->data.assign.left && node->data.assign.left->inferred_type) {
                inferredLLVM = getLLVMTypeFromTypeInfo(node->data.assign.left->inferred_type);
            }
            if (!inferredLLVM && node->data.assign.right && node->data.assign.right->inferred_type) {
                inferredLLVM = getLLVMTypeFromTypeInfo(node->data.assign.right->inferred_type);
            }
            if (inferredLLVM && inferredLLVM != rightVal.value->getType()) {
                // Convert the value to the inferred type (e.g., ptr -> i32 for ADT payloads)
                if (inferredLLVM->isIntegerTy() && rightVal.value->getType()->isPointerTy()) {
                    rightVal.value = builder.CreatePtrToInt(rightVal.value, Type::getInt64Ty(context), "adt_payload_ptrtoint");
                    rightVal.value = builder.CreateIntCast(rightVal.value, inferredLLVM, true, "adt_payload_intcast");
                    rightVal.type = typeHelper.getValueTypeFromType(inferredLLVM);
                }
                varType = inferredLLVM;
            } else {
                varType = rightVal.value->getType();
            }
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
            
            /* Use declared type to determine array element type for empty arrays */
            if (arraySize == 0 && node->data.assign.declared_type) {
                ASTNode* declType = node->data.assign.declared_type;
                if (declType->type == AST_TYPE_LIST) {
                    ASTNode* elemTypeNode = declType->data.list_type.element_type;
                    if (elemTypeNode) {
                        Type* declaredElemType = typeHelper.getTypeFromTypeNode(elemTypeNode);
                        if (declaredElemType) {
                            typeHelper.registerArrayType(name, declaredElemType, 0);
                        }
                    }
                }
            }
            
            if (rightVal.value && rightVal.value->getType()->isPointerTy()) {
                Type* elemType = getInferredArrayElementType(node->data.assign.right);
                bool hasInferredElem = (elemType != nullptr);
                if (!elemType) {
                    elemType = Type::getInt32Ty(context);
                }
                // 结构体在数组中存储为指针
                if (elemType && elemType->isStructTy()) {
                    elemType = PointerType::get(context, 0);
                }
                if (!hasInferredElem && arraySize > 0) {
                    ASTNode* firstElem = node->data.assign.right->data.expression_list.expressions[0];
                    if (firstElem) {
                        if (firstElem->type == AST_STRING) {
                            elemType = PointerType::get(context, 0);
                        } else if (firstElem->type == AST_CHAR) {
                            elemType = Type::getInt8Ty(context);
                        } else if (firstElem->type == AST_NUM_FLOAT) {
                            elemType = Type::getDoubleTy(context);
                        } else if (firstElem->type == AST_NUM_INT) {
                            int64_t v = firstElem->data.num_int.value;
                            elemType = (v >= -2147483648LL && v <= 2147483647LL)
                                       ? Type::getInt32Ty(context)
                                       : Type::getInt64Ty(context);
                        } else if (firstElem->type == AST_STRUCT_LITERAL) {
                            Type* rawType = typeHelper.getTypeFromTypeNode(firstElem->data.struct_literal.type_name);
                            if (rawType && rawType->isStructTy()) {
                                elemType = PointerType::get(context, 0);
                            }
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
            // Propagate the hint to the variable's alloca for ADT struct types
            if (alloc) {
                pointerElementHints[alloc] = hintIt->second;
            }
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
