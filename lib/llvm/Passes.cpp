#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <memory>
#include <mutex>
#include <string>

namespace {

std::string LastError;
std::once_flag InitOnce;

void setError(const std::string &Message) { LastError = Message; }

void initializeTargets() {
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();
}

llvm::OptimizationLevel optimizationLevel(int Level) {
  if (Level <= 0)
    return llvm::OptimizationLevel::O0;
  if (Level == 1)
    return llvm::OptimizationLevel::O1;
  if (Level == 2)
    return llvm::OptimizationLevel::O2;
  return llvm::OptimizationLevel::O3;
}

bool verifyModuleToError(llvm::Module &Module, std::string &Error) {
  Error.clear();
  llvm::raw_string_ostream OS(Error);
  if (!llvm::verifyModule(Module, &OS))
    return false;
  OS.flush();
  return true;
}

int optimizeIrFile(const char *InputPath, const char *OutputPath,
                   const char *TargetTriple, int OptLevel) {
  LastError.clear();
  if (InputPath == nullptr || OutputPath == nullptr || InputPath[0] == '\0' ||
      OutputPath[0] == '\0') {
    setError("missing input or output path");
    return 1;
  }
  if (OptLevel < 0 || OptLevel > 3) {
    setError("optimization level must be between 0 and 3");
    return 1;
  }

  std::call_once(InitOnce, initializeTargets);

  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  std::unique_ptr<llvm::Module> Module =
      llvm::parseIRFile(InputPath, Diagnostic, Context);
  if (!Module) {
    std::string Message;
    llvm::raw_string_ostream OS(Message);
    Diagnostic.print("vixc", OS);
    setError(OS.str());
    return 1;
  }

  std::string TripleText =
      (TargetTriple != nullptr && TargetTriple[0] != '\0')
          ? std::string(TargetTriple)
          : Module->getTargetTriple().str();
  if (TripleText.empty())
    TripleText = llvm::sys::getDefaultTargetTriple();
  llvm::Triple Triple(TripleText);
  Module->setTargetTriple(Triple);

  std::string LookupError;
  const llvm::Target *Target =
      llvm::TargetRegistry::lookupTarget(Triple, LookupError);
  if (Target == nullptr) {
    setError(LookupError);
    return 1;
  }

  llvm::TargetOptions Options;
  std::unique_ptr<llvm::TargetMachine> TM(Target->createTargetMachine(
      Triple, "generic", "", Options, llvm::Reloc::PIC_));
  if (!TM) {
    setError("failed to create LLVM target machine");
    return 1;
  }
  Module->setDataLayout(TM->createDataLayout());

  std::string VerifyError;
  if (verifyModuleToError(*Module, VerifyError)) {
    setError(VerifyError);
    return 1;
  }

  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder PB(TM.get());
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::OptimizationLevel Level = optimizationLevel(OptLevel);
  llvm::ModulePassManager MPM =
      OptLevel == 0 ? PB.buildO0DefaultPipeline(Level)
                    : PB.buildPerModuleDefaultPipeline(Level);
  MPM.run(*Module, MAM);

  if (verifyModuleToError(*Module, VerifyError)) {
    setError(VerifyError);
    return 1;
  }

  std::error_code EC;
  llvm::raw_fd_ostream Out(OutputPath, EC, llvm::sys::fs::OF_Text);
  if (EC) {
    setError("cannot open output file '" + std::string(OutputPath) +
             "': " + EC.message());
    return 1;
  }

  Module->print(Out, nullptr);
  Out.flush();
  return 0;
}

} // namespace

extern "C" {

int vix_passes_optimize_ir(const char *InputPath, const char *OutputPath,
                           const char *TargetTriple, int OptLevel) {
  return optimizeIrFile(InputPath, OutputPath, TargetTriple, OptLevel);
}

const char *vix_passes_last_error(void) { return LastError.c_str(); }

}
