#include <lld/Common/Driver.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/raw_ostream.h>

#include <string>
#include <vector>

LLD_HAS_DRIVER(elf)

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

} // namespace

extern "C" {

int vix_lld_link_elf(const char *ArgsText) {
  LastError.clear();
  std::vector<std::string> Storage;
  Storage.push_back("ld.lld");
  addSplitArgs(Storage, ArgsText);
  return runElfLink(Storage);
}

const char *vix_lld_last_error(void) { return LastError.c_str(); }

}
