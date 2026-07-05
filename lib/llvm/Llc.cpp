#include <llvm/ADT/StringRef.h>
#include <llvm/CodeGen/CommandFlags.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

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

llvm::CodeGenFileType fileTypeForMode(int Mode) {
  if (Mode == 1)
    return llvm::CodeGenFileType::AssemblyFile;
  return llvm::CodeGenFileType::ObjectFile;
}

int compileIrFile(const char *InputPath, const char *OutputPath,
                  const char *TargetTriple, int Mode) {
  LastError.clear();
  if (InputPath == nullptr || OutputPath == nullptr || InputPath[0] == '\0' ||
      OutputPath[0] == '\0') {
    setError("missing input or output path");
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

  std::error_code EC;
  llvm::raw_fd_ostream Out(OutputPath, EC, llvm::sys::fs::OF_None);
  if (EC) {
    setError("cannot open output file '" + std::string(OutputPath) +
             "': " + EC.message());
    return 1;
  }

  llvm::legacy::PassManager PM;
  if (TM->addPassesToEmitFile(PM, Out, nullptr, fileTypeForMode(Mode))) {
    setError("target does not support requested output kind");
    return 1;
  }

  PM.run(*Module);
  Out.flush();
  return 0;
}

} // namespace

extern "C" {

int vix_llc_compile_ir_to_object(const char *InputPath, const char *OutputPath,
                                 const char *TargetTriple) {
  return compileIrFile(InputPath, OutputPath, TargetTriple, 0);
}

int vix_llc_compile_ir_to_asm(const char *InputPath, const char *OutputPath,
                              const char *TargetTriple) {
  return compileIrFile(InputPath, OutputPath, TargetTriple, 1);
}

const char *vix_llc_last_error(void) { return LastError.c_str(); }

}
