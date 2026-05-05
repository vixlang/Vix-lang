#include <llvm/IR/Module.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static OptimizationLevel toLLVMLevel(int level) {
    switch (level) {
        case 1:  return OptimizationLevel::O1;
        case 2:  return OptimizationLevel::O2;
        case 3:  return OptimizationLevel::O3;
        default: return OptimizationLevel::O0;
    }
}

static std::unique_ptr<TargetMachine> createHostTM() {
    InitializeAllTargetInfos();
    InitializeAllTargets();

    std::string triple = sys::getDefaultTargetTriple();
    std::string error;
    const Target* target = TargetRegistry::lookupTarget(triple, error);
    if (!target) return nullptr;

    TargetOptions opts;
    return std::unique_ptr<TargetMachine>(
        target->createTargetMachine(triple, "generic", "", opts, Reloc::PIC_));
}

extern "C" void vix_optimize_module(void* llvm_module, int level) {
    if (!llvm_module || level <= 0) return;

    Module* M = static_cast<Module*>(llvm_module);

    auto TM = createHostTM();
    if (!TM) return;

    M->setDataLayout(TM->createDataLayout());
    M->setTargetTriple(TM->getTargetTriple().str());

    PassBuilder PB(TM.get());

    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;

    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);

    OptimizationLevel OL = toLLVMLevel(level);
    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OL);
    MPM.run(*M, MAM);
}
