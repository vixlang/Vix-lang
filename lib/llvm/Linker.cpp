#include <lld/Common/Driver.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(WIN32)
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

namespace llvm {
class MemoryBuffer;
class MemoryBufferRef;

namespace windows_manifest {

/* The LLVM release tarball for Windows ships lldCOFF.lib but not the
 * libxml2 support library that LLVMWindowsManifest.lib depends on.  These
 * definitions stand in for that library so the COFF driver links.  With
 * isAvailable() == false lld never invokes the merger (it defers to an
 * external mt.exe when a /MANIFEST link is explicitly requested, which the
 * Vix driver never does).  The private member is declared as a raw pointer
 * solely so the destructor can be defined here; the size and alignment
 * match the unique_ptr used by the real library. */
bool isAvailable();

class WindowsManifestMerger {
public:
  WindowsManifestMerger();
  ~WindowsManifestMerger();
  Error merge(MemoryBufferRef Manifest);
  std::unique_ptr<MemoryBuffer> getMergedManifest();

private:
  class WindowsManifestMergerImpl;
  WindowsManifestMergerImpl *Impl;
};

bool isAvailable() { return false; }
WindowsManifestMerger::WindowsManifestMerger() : Impl(nullptr) {}
WindowsManifestMerger::~WindowsManifestMerger() {}
Error WindowsManifestMerger::merge(MemoryBufferRef) {
  return Error::success();
}
std::unique_ptr<MemoryBuffer>
WindowsManifestMerger::getMergedManifest() {
  return nullptr;
}

} // namespace windows_manifest
} // namespace llvm
#endif

LLD_HAS_DRIVER(elf)
#if defined(_WIN32) || defined(WIN32)
LLD_HAS_DRIVER(coff)
#endif

namespace {

std::string LastError;

void addSplitArgs(std::vector<std::string> &Storage, const char *Text) {
  if (Text == nullptr)
    return;

  std::string Current;
  char Quote = 0;
  for (const char *P = Text; *P != '\0'; ++P) {
    char C = *P;
    if (Quote != 0) {
      if (C == Quote) {
        Quote = 0;
      } else {
        Current.push_back(C);
      }
      continue;
    }
    if (C == '\'' || C == '"') {
      Quote = C;
      continue;
    }
    if (C == ' ' || C == '\t' || C == '\n' || C == '\r') {
      if (!Current.empty()) {
        Storage.push_back(Current);
        Current.clear();
      }
      continue;
    }
    Current.push_back(C);
  }
  if (!Current.empty())
    Storage.push_back(Current);
}

std::string joinedArgs(const std::vector<std::string> &Storage) {
  std::string Out;
  for (const std::string &Arg : Storage) {
    if (!Out.empty())
      Out.push_back(' ');
    Out += Arg;
  }
  return Out;
}

int runElfLink(const std::vector<std::string> &Storage) {
  std::vector<const char *> Args;
  Args.reserve(Storage.size());
  for (const std::string &Arg : Storage)
    Args.push_back(Arg.c_str());

  std::string StdoutText;
  std::string StderrText;
  llvm::raw_string_ostream StdoutOS(StdoutText);
  llvm::raw_string_ostream StderrOS(StderrText);
  lld::DriverDef Drivers[] = {{lld::Gnu, &lld::elf::link}};
  lld::Result Result = lld::lldMain(Args, StdoutOS, StderrOS, Drivers);
  StdoutOS.flush();
  StderrOS.flush();
  LastError = StderrText;
  if (LastError.empty())
    LastError = StdoutText;
  if (Result.retCode != 0)
    LastError = "lld args: " + joinedArgs(Storage) + "\n" + LastError;
  return Result.retCode;
}

#if defined(_WIN32) || defined(WIN32)
int runCoffLink(const std::vector<std::string> &Storage) {
  std::vector<const char *> Args;
  Args.reserve(Storage.size());
  for (const std::string &Arg : Storage)
    Args.push_back(Arg.c_str());

  std::string StdoutText;
  std::string StderrText;
  llvm::raw_string_ostream StdoutOS(StdoutText);
  llvm::raw_string_ostream StderrOS(StderrText);
  lld::DriverDef Drivers[] = {{lld::WinLink, &lld::coff::link}};
  lld::Result Result = lld::lldMain(Args, StdoutOS, StderrOS, Drivers);
  StdoutOS.flush();
  StderrOS.flush();
  LastError = StderrText;
  if (LastError.empty())
    LastError = StdoutText;
  if (Result.retCode != 0)
    LastError = "lld args: " + joinedArgs(Storage) + "\n" + LastError;
  return Result.retCode;
}
#endif

} // namespace

extern "C" {

int vix_lld_link_elf(const char *ArgsText) {
  LastError.clear();
  std::vector<std::string> Storage;
  Storage.push_back("ld.lld");
  addSplitArgs(Storage, ArgsText);
  return runElfLink(Storage);
}

#if defined(_WIN32) || defined(WIN32)
int vix_lld_link_coff(const char *ArgsText) {
  LastError.clear();
  std::vector<std::string> Storage;
  Storage.push_back("lld-link");
  addSplitArgs(Storage, ArgsText);
  return runCoffLink(Storage);
}
#endif

const char *vix_lld_last_error(void) { return LastError.c_str(); }

}
