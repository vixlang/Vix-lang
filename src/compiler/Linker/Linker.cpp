/*
 * Copyright (c) 2026 Vix Language Authors. All rights reserved.
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
#include "Linker.h"

#include <lld/Common/Driver.h>
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(mingw)
LLD_HAS_DRIVER(wasm)

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <string>
#include <set>
#include <vector>

using namespace llvm;

namespace {

enum class LinkFlavor { ELF, MachO, COFF, MinGW, Wasm };

LinkFlavor detectFlavor(const Triple &T) {
    if (T.isOSBinFormatMachO()) return LinkFlavor::MachO;
    if (T.isOSBinFormatWasm())  return LinkFlavor::Wasm;
    if (T.isOSBinFormatCOFF()) {
        if (T.isWindowsGNUEnvironment()) return LinkFlavor::MinGW;
        return LinkFlavor::COFF;
    }
    return LinkFlavor::ELF;
}

bool fileExists(const Twine &path) {
    return sys::fs::exists(path);
}

// ── Linux / ELF sysroot discovery ───────────────────────────────
struct SysPaths {
    std::string gccDir;
    std::string sysLibDir;
    std::string dynamicLinker;
};

SysPaths probeSysPaths(const Triple &T) {
    SysPaths sp;

    StringRef arch = T.isArch64Bit() ? "x86_64" : (T.getArch() == Triple::aarch64 ? "aarch64" : "x86_64");
    
    // Try multiple GNU tuple variants (e.g. x86_64-linux-gnu, x86_64-pc-linux-gnu)
    std::string gnuTuple = arch.str() + "-linux-gnu";
    std::string gnuTuplePc = arch.str() + "-pc-linux-gnu";
    
    auto findLibDir = [&](const std::string &tuple) -> std::string {
        std::string dir = "/usr/lib/" + tuple;
        if (fileExists(dir + "/crt1.o"))
            return dir;
        return {};
    };
    
    auto findGccDir = [&](const std::string &tuple) -> std::string {
        SmallString<256> gccBase("/usr/lib/gcc/" + tuple);
        if (sys::fs::is_directory(gccBase)) {
            std::string bestVer;
            std::error_code ec;
            for (sys::fs::directory_iterator it(gccBase, ec), end; it != end;
                 it.increment(ec)) {
                StringRef name = sys::path::filename(it->path());
                if (name > bestVer)
                    bestVer = name.str();
            }
            if (!bestVer.empty()) {
                std::string cand = (gccBase + "/" + bestVer).str();
                if (fileExists(cand + "/crtbegin.o"))
                    return cand;
            }
        }
        return {};
    };

    // Try standard tuple first, then -pc- variant
    sp.sysLibDir = findLibDir(gnuTuple);
    if (sp.sysLibDir.empty())
        sp.sysLibDir = findLibDir(gnuTuplePc);
    if (sp.sysLibDir.empty()) {
        std::string alt = "/usr/lib/" + std::string(T.isArch64Bit() ? "64" : "32");
        if (fileExists(alt + "/crt1.o"))
            sp.sysLibDir = alt;
    }

    sp.gccDir = findGccDir(gnuTuple);
    if (sp.gccDir.empty())
        sp.gccDir = findGccDir(gnuTuplePc);

    static const char *const ldCandidates[] = {
        "/lib64/ld-linux-x86-64.so.2",
        "/lib/ld-linux-aarch64.so.1",
        "/lib/ld-linux-armhf.so.3",
        "/lib/ld-linux.so.2",
        "/lib/ld-musl-x86_64.so.1",
    };
    for (auto *c : ldCandidates) {
        if (fileExists(c)) {
            sp.dynamicLinker = c;
            break;
        }
    }

    return sp;
}

// ── macOS SDK discovery ─────────────────────────────────────────
std::string findMacOSSDK() {
    static const char *const sdkCandidates[] = {
        "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
        "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
    };
    for (auto *c : sdkCandidates) {
        if (sys::fs::is_directory(c))
            return c;
    }
    return {};
}

static bool findLibInDirs(const std::string &libName,
                          const std::vector<std::string> &dirs,
                          std::string &foundPath) {
    std::string candidates[] = {
        "lib" + libName + ".a",
        "lib" + libName + ".dll.a",
        libName + ".lib",
        libName + ".a",
    };
    for (auto &dir : dirs) {
        for (auto &cand : candidates) {
            std::string path = dir + "/" + cand;
            if (fileExists(path)) {
                foundPath = path;
                return true;
            }
        }
    }
    return false;
}
static std::vector<std::string> probeWindowsSDKPaths() {
    std::vector<std::string> paths;
#ifdef _WIN32
    static const char *const sdkRoots[] = {
        "C:/Program Files (x86)/Windows Kits/10/Lib",
        "C:/Program Files/Windows Kits/10/Lib",
        NULL
    };
    for (size_t i = 0; sdkRoots[i]; i++) {
        auto *root = sdkRoots[i];
        bool is_dir;
        std::error_code ec = sys::fs::is_directory(root, is_dir);
        if (!is_dir) continue;
        std::string bestVer;
        for (sys::fs::directory_iterator it(root, ec), end; it != end;
             it.increment(ec)) {
            std::string name = sys::path::filename(it->path()).str();
            if (name > bestVer) bestVer = name;
        }
        if (bestVer.empty()) continue;
        std::string base = std::string(root) + "/" + bestVer;
        static const char *const subdirs[] = {
            "/ucrt/x64", "/um/x64",
            "/ucrt/x86", "/um/x86",
            "/ucrt/arm64", "/um/arm64",
            NULL
        };
        for (size_t i2 = 0; subdirs[i2]; i2++) {
            std::string p = base + subdirs[i2];
            bool exists;
            std::error_code ec2 = sys::fs::is_directory(p, exists);
            if (exists)
                paths.push_back(p);
        }
    }
    static const char *const vsRoots[] = {
        "C:/Program Files/Microsoft Visual Studio",
        "C:/Program Files (x86)/Microsoft Visual Studio",
        NULL
    };
    static const char *const standaloneTools[] = {
        "C:/BuildTools/VC/Tools/MSVC",
        "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC",
        "C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/MSVC",
        "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC",
        "C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Tools/MSVC",
        "C:/Program Files (x86)/Microsoft Visual Studio/2019/Professional/VC/Tools/MSVC",
        "C:/Program Files (x86)/Microsoft Visual Studio/2019/Enterprise/VC/Tools/MSVC",
        "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC",
        "C:/Program Files (x86)/Microsoft Visual Studio/2019/BuildTools/VC/Tools/MSVC",
        NULL
    };
    for (size_t i = 0; standaloneTools[i]; i++) {
        auto *toolsDir = standaloneTools[i];
        bool exists;
        std::error_code ec2 = sys::fs::is_directory(toolsDir, exists);
        if (!exists) continue;
        std::string bestVer;
        std::error_code ec;
        for (sys::fs::directory_iterator it(toolsDir, ec), end; it != end;
             it.increment(ec)) {
            StringRef name = sys::path::filename(it->path());
            if (name > bestVer) bestVer = name.str();
        }
        if (bestVer.empty()) continue;
        std::string libBase = std::string(toolsDir) + "/" + bestVer + "/lib/x64";
        if (sys::fs::is_directory(libBase))
            paths.push_back(libBase);
        std::string libBaseArm = std::string(toolsDir) + "/" + bestVer + "/lib/arm64";
        if (sys::fs::is_directory(libBaseArm))
            paths.push_back(libBaseArm);
        std::string libBaseX86 = std::string(toolsDir) + "/" + bestVer + "/lib/x86";
        if (sys::fs::is_directory(libBaseX86))
            paths.push_back(libBaseX86);
    }
#endif
    return paths;
}

// ── MinGW installation discovery ────────────────────────────────
struct MinGWPaths {
    std::string libDir;    // e.g. /usr/x86_64-w64-mingw32/lib
    std::string crtDir;    // e.g. /usr/lib/gcc/x86_64-w64-mingw32/13-posix
};

MinGWPaths probeMinGWPaths(const Triple &T) {
    MinGWPaths mp;
    std::string triple = T.getTriple(); // e.g. x86_64-w64-mingw32

    // Standard MinGW sysroot locations (Linux cross-compile).
    std::string sysroot = "/usr/" + triple;
    if (sys::fs::is_directory(sysroot + "/lib")) {
        mp.libDir = sysroot + "/lib";
    }

    // GCC cross-compiler CRT directory.
    std::string gccBase = "/usr/lib/gcc/" + triple;
    if (sys::fs::is_directory(gccBase)) {
        std::string bestVer;
        std::error_code ec;
        for (sys::fs::directory_iterator it(gccBase, ec), end; it != end;
             it.increment(ec)) {
            StringRef name = sys::path::filename(it->path());
            if (name > bestVer)
                bestVer = name.str();
        }
        if (!bestVer.empty()) {
            std::string cand = gccBase + "/" + bestVer;
            // Prefer posix threading variant if available.
            if (sys::fs::is_directory(cand + "-posix"))
                cand += "-posix";
            if (fileExists(cand + "/crtbegin.o"))
                mp.crtDir = cand;
        }
    }
#ifdef _WIN32
    if (mp.libDir.empty()) {
        static const char *const winCandidates[] = {
            "C:/mingw64/lib",
            "C:/mingw32/lib",
            "C:/msys64/mingw64/lib",
            "C:/msys64/mingw32/lib",
            "C:/msys64/ucrt64/lib",
            "C:/mingw64/x86_64-w64-mingw32/lib",
            "C:/mingw32/i686-w64-mingw32/lib",
            NULL
        };
        for (const char *const *c = winCandidates; *c; ++c) {
            if (sys::fs::is_directory(*c)) {
                mp.libDir = *c;
                break;
            }
        }
    }
    if (mp.crtDir.empty()) {
        static const char *const winCrtCandidates[] = {
            "C:/mingw64/lib/gcc/x86_64-w64-mingw32",
            "C:/mingw32/lib/gcc/i686-w64-mingw32",
            "C:/msys64/mingw64/lib/gcc/x86_64-w64-mingw32",
            "C:/msys64/ucrt64/lib/gcc/x86_64-w64-mingw32",
            NULL
        };
        for (const char *const *c = winCrtCandidates; *c; ++c) {
            if (sys::fs::is_directory(*c)) {
                std::string bestVer;
                std::error_code ec;
                for (sys::fs::directory_iterator it(*c, ec), end; it != end;
                     it.increment(ec)) {
                    StringRef name = sys::path::filename(it->path());
                    if (name > bestVer)
                        bestVer = name.str();
                }
                if (!bestVer.empty()) {
                    std::string cand = std::string(*c) + "/" + bestVer;
                    if (sys::fs::is_directory(cand + "-posix"))
                        cand += "-posix";
                    if (fileExists(cand + "/crtbegin.o"))
                        mp.crtDir = cand;
                }
                if (!mp.crtDir.empty()) break;
            }
        }
    }
#endif

    return mp;
}

// ── Linker argument builders ────────────────────────────────────

void addBareArgs(std::vector<std::string> &args, const char *entry,
                 const char *script) {
    args.push_back("-static");
    args.push_back("-e");
    args.push_back(entry && entry[0] ? entry : "_start");
    if (script && script[0]) {
        args.push_back("-T");
        args.push_back(script);
    }
}

void buildElfArgs(std::vector<std::string> &args, bool bare,
                  const char *entry, const char *script) {
    if (bare) {
        args.push_back("--no-dynamic-linker");
        args.push_back("-z");
        args.push_back("max-page-size=0x1000");
        args.push_back("--build-id=none");
        addBareArgs(args, entry, script);
    }
}

void buildMachOArgs(std::vector<std::string> &args, const Triple &T,
                    bool bare, const char *entry, const char *script) {
    args.push_back("-arch");
    args.push_back(T.getArch() == Triple::aarch64 ? "arm64" : "x86_64");

    if (!bare) {
        std::string sdk = findMacOSSDK();
        if (!sdk.empty()) {
            args.push_back("-syslibroot");
            args.push_back(sdk);
        }
        args.push_back("-L/usr/lib");
        args.push_back("-L/usr/local/lib");
        if (!sdk.empty())
            args.push_back("-L" + sdk + "/usr/lib");

        args.push_back("-lSystem");
        args.push_back("-lc++");
    } else {
        addBareArgs(args, entry, script);
    }
}

void buildCoffArgs(std::vector<std::string> &args, bool bare,
                   const char *entry, const char *script,
                   const char *libc_dir) {
    args.push_back("/NOLOGO");
    if (bare) {
        std::string e = "/ENTRY:";
        e += (entry && entry[0]) ? entry : "main";
        args.push_back(e);
        args.push_back("/SUBSYSTEM:CONSOLE");
        addBareArgs(args, entry, script);
    } else {
        args.push_back("/ENTRY:mainCRTStartup");
        args.push_back("/SUBSYSTEM:CONSOLE");
        // Prefer bundled libc if available
        if (libc_dir && libc_dir[0]) {
            args.push_back("/LIBPATH:" + std::string(libc_dir));
        }
        // Add Windows SDK and MSVC library paths
        auto sdkPaths = probeWindowsSDKPaths();
        for (auto &p : sdkPaths)
            args.push_back("/LIBPATH:" + p);
        args.push_back("legacy_stdio_definitions.lib");
        args.push_back("vcruntime.lib");
        args.push_back("ucrt.lib");
        args.push_back("msvcrt.lib");
        args.push_back("kernel32.lib");
        args.push_back("advapi32.lib");
        args.push_back("user32.lib");
        args.push_back("shell32.lib");
    }
}

void buildMinGWArgs(std::vector<std::string> &args, const Triple &T,
                    bool bare, const char *entry, const char *script,
                    const char *libc_dir) {
    if (bare) {
        args.push_back("--no-dynamic-linker");
        args.push_back("-e");
        args.push_back(entry && entry[0] ? entry : "main");
        args.push_back("-static");
        if (script && script[0]) {
            args.push_back("-T");
            args.push_back(script);
        }
    } else {
        // Prefer bundled libc if available
        bool haveBundled = libc_dir && libc_dir[0] &&
                           sys::fs::is_directory(libc_dir);
        MinGWPaths mp = probeMinGWPaths(T);

        // Collect all searchable library directories
        std::vector<std::string> libSearchDirs;
        if (haveBundled)
            libSearchDirs.push_back(libc_dir);
        if (!mp.libDir.empty())
            libSearchDirs.push_back(mp.libDir);
        if (!mp.crtDir.empty())
            libSearchDirs.push_back(mp.crtDir);

        // On Windows, also add Windows SDK and MSVC paths
        auto sdkPaths = probeWindowsSDKPaths();
        for (auto &p : sdkPaths)
            libSearchDirs.push_back(p);

        // Add all search directories as -L flags
        std::set<std::string> addedPaths;
        auto addSearchPath = [&](const std::string &dir) {
            if (!dir.empty() && addedPaths.insert(dir).second)
                args.push_back("-L" + dir);
        };
        for (auto &d : libSearchDirs)
            addSearchPath(d);

        if (haveBundled) {
            // Try bundled CRT objects
            std::string crtbegin = std::string(libc_dir) + "/crtbegin.o";
            if (fileExists(crtbegin))
                args.push_back(crtbegin);
            std::string crt2 = std::string(libc_dir) + "/crt2.o";
            if (fileExists(crt2))
                args.push_back(crt2);
        } else {
            if (!mp.crtDir.empty() && fileExists(mp.crtDir + "/crtbegin.o"))
                args.push_back(mp.crtDir + "/crtbegin.o");
        }

        // Always-required MinGW core libraries
        args.push_back("-lmingw32");
        args.push_back("-lgcc");
        args.push_back("-lgcc_eh");

        // Conditionally add libraries that may or may not be present
        static const char *const optionalLibs[] = {
            "moldname", "mingwex", "msvcrt", "kernel32",
            "pthread", "advapi32", "shell32", "user32",
            "ucrt", "mingw32",
        };
        for (auto *lib : optionalLibs) {
            std::string foundPath;
            if (findLibInDirs(lib, libSearchDirs, foundPath)) {
                args.push_back(std::string("-l") + lib);
            }
        }

        // Second pass of gcc libs (required by MinGW convention)
        args.push_back("-lgcc");
        args.push_back("-lgcc_eh");

        // Ensure msvcrt is linked (critical for MinGW)
        {
            std::string foundPath;
            if (!findLibInDirs("msvcrt", libSearchDirs, foundPath)) {
                // Fallback: still request it, lld may find it via default paths
                args.push_back("-lmsvcrt");
            }
        }

        if (haveBundled) {
            std::string crtend = std::string(libc_dir) + "/crtend.o";
            if (fileExists(crtend))
                args.push_back(crtend);
        } else {
            if (!mp.crtDir.empty() && fileExists(mp.crtDir + "/crtend.o"))
                args.push_back(mp.crtDir + "/crtend.o");
        }
    }
}

} // namespace

extern "C" int vix_link(const char *obj_file, const char *output_file,
                        const VixLinkOptions *options, const char **error_msg) {
    static thread_local std::string lastError;

    if (!obj_file || !output_file) {
        lastError = "linker: missing input or output file";
        if (error_msg) *error_msg = lastError.c_str();
        return 0;
    }

    std::string tripleStr = (options && options->target_triple && options->target_triple[0])
                                ? options->target_triple
                                : sys::getDefaultTargetTriple();
    Triple T(tripleStr);
    LinkFlavor flavor = detectFlavor(T);
    bool bare = options && options->bare_mode;

    std::vector<std::string> args;
    const char *progName = nullptr;
    switch (flavor) {
        case LinkFlavor::ELF:   progName = "ld.lld";    break;
        case LinkFlavor::MachO: progName = "ld64.lld";  break;
        case LinkFlavor::COFF:  progName = "lld-link";  break;
        case LinkFlavor::MinGW: progName = "ld.lld";    break;
        case LinkFlavor::Wasm:  progName = "wasm-ld";   break;
    }
    args.push_back(progName);

    const char *entry = options ? options->entry_point : nullptr;
    const char *script = options ? options->linker_script : nullptr;
    const char *libc_dir = options ? options->libc_dir : nullptr;

    switch (flavor) {
        case LinkFlavor::ELF:
            buildElfArgs(args, bare, entry, script);
            break;
        case LinkFlavor::MachO:
            buildMachOArgs(args, T, bare, entry, script);
            break;
        case LinkFlavor::COFF:
            buildCoffArgs(args, bare, entry, script, libc_dir);
            break;
        case LinkFlavor::MinGW:
            buildMinGWArgs(args, T, bare, entry, script, libc_dir);
            break;
        case LinkFlavor::Wasm:
            if (bare)
                addBareArgs(args, entry, script);
            break;
    }

    // ── ELF sysroot (Linux only, non-bare) ────────────────────
    SysPaths elfSysPaths;
    bool wantStatic = options && options->static_link;
    if (flavor == LinkFlavor::ELF && !bare) {
        elfSysPaths = probeSysPaths(T);

        if (wantStatic) {
            args.push_back("-static");
            args.push_back("--exclude-libs=ALL");
            args.push_back("--defsym");
            args.push_back("_DYNAMIC=0");
        }

        if (!elfSysPaths.gccDir.empty() && fileExists(elfSysPaths.gccDir + "/crtbegin.o"))
            args.push_back(elfSysPaths.gccDir + "/crtbegin.o");
        if (!elfSysPaths.sysLibDir.empty()) {
            if (!wantStatic && fileExists(elfSysPaths.sysLibDir + "/crt1.o"))
                args.push_back(elfSysPaths.sysLibDir + "/crt1.o");
            if (fileExists(elfSysPaths.sysLibDir + "/crti.o"))
                args.push_back(elfSysPaths.sysLibDir + "/crti.o");
        }
        if (!wantStatic && !elfSysPaths.dynamicLinker.empty()) {
            args.push_back("--dynamic-linker");
            args.push_back(elfSysPaths.dynamicLinker);
        }
        if (!elfSysPaths.sysLibDir.empty())
            args.push_back("-L" + elfSysPaths.sysLibDir);
        if (!elfSysPaths.gccDir.empty())
            args.push_back("-L" + elfSysPaths.gccDir);
        args.push_back("-L/lib");
        args.push_back("-L/usr/lib");
    }

    // ── User-specified library paths (-L /LIBPATH:) ───────────
    if (options && options->lib_paths && options->lib_path_count > 0) {
        for (int i = 0; i < options->lib_path_count; i++) {
            if (options->lib_paths[i] && options->lib_paths[i][0]) {
                if (flavor == LinkFlavor::COFF)
                    args.push_back("/LIBPATH:" + std::string(options->lib_paths[i]));
                else
                    args.push_back("-L" + std::string(options->lib_paths[i]));
            }
        }
    }

    // ── Input / output ────────────────────────────────────────
    args.push_back(obj_file);
    if (flavor == LinkFlavor::COFF) {
        args.push_back("/out:" + std::string(output_file));
    } else {
        args.push_back("-o");
        args.push_back(output_file);
    }

    // ── System libraries (ELF non-bare only, others handled above) ─
    if (!bare && flavor == LinkFlavor::ELF) {
        if (wantStatic) {
            args.push_back("-lgcc");
            args.push_back("-lgcc_eh");
        }
        args.push_back("-lc");
        args.push_back("-lm");
        args.push_back("-ldl");
        args.push_back("-lpthread");
        args.push_back("-lstdc++");
        if (wantStatic) {
            args.push_back("-lgcc");
            args.push_back("-lgcc_eh");
        }
        if (!elfSysPaths.gccDir.empty() && fileExists(elfSysPaths.gccDir + "/crtend.o"))
            args.push_back(elfSysPaths.gccDir + "/crtend.o");
        if (!elfSysPaths.sysLibDir.empty() && fileExists(elfSysPaths.sysLibDir + "/crtn.o"))
            args.push_back(elfSysPaths.sysLibDir + "/crtn.o");
    }

    // ── Extra libraries from -l flags ─────────────────────────
    if (options && options->extra_libs && options->extra_lib_count > 0) {
        for (int i = 0; i < options->extra_lib_count; i++) {
            args.push_back(std::string("-l") + options->extra_libs[i]);
        }
    }

    // ── Convert to const char* array for LLD API ──────────────
    std::vector<const char *> rawArgs;
    rawArgs.reserve(args.size());
    for (auto &s : args)
        rawArgs.push_back(s.c_str());



    std::string outStr, errStr;
    raw_string_ostream outOS(outStr);
    raw_string_ostream errOS(errStr);

    // ── Select LLD driver ─────────────────────────────────────
    std::vector<lld::DriverDef> drivers;
    switch (flavor) {
        case LinkFlavor::ELF:
            drivers.push_back({lld::Gnu, lld::elf::link});
            break;
        case LinkFlavor::MachO:
            drivers.push_back({lld::Darwin, lld::macho::link});
            break;
        case LinkFlavor::COFF:
            drivers.push_back({lld::WinLink, lld::coff::link});
            break;
        case LinkFlavor::MinGW:
            drivers.push_back({lld::MinGW, lld::mingw::link});
            break;
        case LinkFlavor::Wasm:
            drivers.push_back({lld::Wasm, lld::wasm::link});
            break;
    }

    lld::Result result = lld::lldMain(rawArgs, outOS, errOS, drivers);

    outOS.flush();
    errOS.flush();

    if (result.retCode != 0) {
        lastError = errStr.empty()
                        ? "linker failed with exit code " + std::to_string(result.retCode)
                        : errStr;
        if (error_msg) *error_msg = lastError.c_str();
        return 0;
    }

    if (error_msg) *error_msg = nullptr;
    return 1;
}

extern "C" int vix_link_multi(const char **obj_files, int obj_count,
                              const char *output_file,
                              const VixLinkOptions *options,
                              const char **error_msg) {
    static thread_local std::string lastError;

    if (!obj_files || obj_count <= 0 || !output_file) {
        lastError = "linker: missing input or output file";
        if (error_msg) *error_msg = lastError.c_str();
        return 0;
    }

    std::string tripleStr = (options && options->target_triple && options->target_triple[0])
                                ? options->target_triple
                                : sys::getDefaultTargetTriple();
    Triple T(tripleStr);
    LinkFlavor flavor = detectFlavor(T);
    bool bare = options && options->bare_mode;

    std::vector<std::string> args;
    const char *progName = nullptr;
    switch (flavor) {
        case LinkFlavor::ELF:   progName = "ld.lld";    break;
        case LinkFlavor::MachO: progName = "ld64.lld";  break;
        case LinkFlavor::COFF:  progName = "lld-link";  break;
        case LinkFlavor::MinGW: progName = "ld.lld";    break;
        case LinkFlavor::Wasm:  progName = "wasm-ld";   break;
    }
    args.push_back(progName);

    const char *entry = options ? options->entry_point : nullptr;
    const char *script = options ? options->linker_script : nullptr;
    const char *libc_dir = options ? options->libc_dir : nullptr;

    switch (flavor) {
        case LinkFlavor::ELF:
            buildElfArgs(args, bare, entry, script);
            break;
        case LinkFlavor::MachO:
            buildMachOArgs(args, T, bare, entry, script);
            break;
        case LinkFlavor::COFF:
            buildCoffArgs(args, bare, entry, script, libc_dir);
            break;
        case LinkFlavor::MinGW:
            buildMinGWArgs(args, T, bare, entry, script, libc_dir);
            break;
        case LinkFlavor::Wasm:
            if (bare)
                addBareArgs(args, entry, script);
            break;
    }

    SysPaths elfSysPaths;
    bool wantStatic = options && options->static_link;
    if (flavor == LinkFlavor::ELF && !bare) {
        elfSysPaths = probeSysPaths(T);

        if (wantStatic) {
            args.push_back("-static");
            args.push_back("--exclude-libs=ALL");
            args.push_back("--defsym");
            args.push_back("_DYNAMIC=0");
        }

        if (!elfSysPaths.gccDir.empty() && fileExists(elfSysPaths.gccDir + "/crtbegin.o"))
            args.push_back(elfSysPaths.gccDir + "/crtbegin.o");
        if (!elfSysPaths.sysLibDir.empty()) {
            if (!wantStatic && fileExists(elfSysPaths.sysLibDir + "/crt1.o"))
                args.push_back(elfSysPaths.sysLibDir + "/crt1.o");
            if (fileExists(elfSysPaths.sysLibDir + "/crti.o"))
                args.push_back(elfSysPaths.sysLibDir + "/crti.o");
        }
        if (!wantStatic && !elfSysPaths.dynamicLinker.empty()) {
            args.push_back("--dynamic-linker");
            args.push_back(elfSysPaths.dynamicLinker);
        }
        if (!elfSysPaths.sysLibDir.empty())
            args.push_back("-L" + elfSysPaths.sysLibDir);
        if (!elfSysPaths.gccDir.empty())
            args.push_back("-L" + elfSysPaths.gccDir);
        args.push_back("-L/lib");
        args.push_back("-L/usr/lib");
    }

    // ── User-specified library paths (-L /LIBPATH:) ───────────
    if (options && options->lib_paths && options->lib_path_count > 0) {
        for (int i = 0; i < options->lib_path_count; i++) {
            if (options->lib_paths[i] && options->lib_paths[i][0]) {
                if (flavor == LinkFlavor::COFF)
                    args.push_back("/LIBPATH:" + std::string(options->lib_paths[i]));
                else
                    args.push_back("-L" + std::string(options->lib_paths[i]));
            }
        }
    }

    for (int i = 0; i < obj_count; i++) {
        args.push_back(obj_files[i]);
    }
    if (flavor == LinkFlavor::COFF) {
        args.push_back("/out:" + std::string(output_file));
    } else {
        args.push_back("-o");
        args.push_back(output_file);
    }

    if (!bare && flavor == LinkFlavor::ELF) {
        if (wantStatic) {
            args.push_back("-lgcc");
            args.push_back("-lgcc_eh");
        }
        args.push_back("-lc");
        args.push_back("-lm");
        args.push_back("-ldl");
        args.push_back("-lpthread");
        args.push_back("-lstdc++");
        if (wantStatic) {
            args.push_back("-lgcc");
            args.push_back("-lgcc_eh");
        }
        if (!elfSysPaths.gccDir.empty() && fileExists(elfSysPaths.gccDir + "/crtend.o"))
            args.push_back(elfSysPaths.gccDir + "/crtend.o");
        if (!elfSysPaths.sysLibDir.empty() && fileExists(elfSysPaths.sysLibDir + "/crtn.o"))
            args.push_back(elfSysPaths.sysLibDir + "/crtn.o");
    }

    // ── Extra libraries from -l flags ─────────────────────────
    if (options && options->extra_libs && options->extra_lib_count > 0) {
        for (int i = 0; i < options->extra_lib_count; i++) {
            args.push_back(std::string("-l") + options->extra_libs[i]);
        }
    }

    std::vector<const char *> rawArgs;
    rawArgs.reserve(args.size());
    for (auto &s : args)
        rawArgs.push_back(s.c_str());

    std::string outStr, errStr;
    raw_string_ostream outOS(outStr);
    raw_string_ostream errOS(errStr);

    std::vector<lld::DriverDef> drivers;
    switch (flavor) {
        case LinkFlavor::ELF:
            drivers.push_back({lld::Gnu, lld::elf::link});
            break;
        case LinkFlavor::MachO:
            drivers.push_back({lld::Darwin, lld::macho::link});
            break;
        case LinkFlavor::COFF:
            drivers.push_back({lld::WinLink, lld::coff::link});
            break;
        case LinkFlavor::MinGW:
            drivers.push_back({lld::MinGW, lld::mingw::link});
            break;
        case LinkFlavor::Wasm:
            drivers.push_back({lld::Wasm, lld::wasm::link});
            break;
    }

    lld::Result result = lld::lldMain(rawArgs, outOS, errOS, drivers);

    outOS.flush();
    errOS.flush();

    if (result.retCode != 0) {
        lastError = errStr.empty()
                        ? "linker failed with exit code " + std::to_string(result.retCode)
                        : errStr;
        if (error_msg) *error_msg = lastError.c_str();
        return 0;
    }

    if (error_msg) *error_msg = nullptr;
    return 1;
}
