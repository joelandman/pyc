#include "pyc/Compiler.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    // Check for --help first
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--help") {
            std::cerr << "Usage: pyc <input.py> [-o output] [--static] [-O0|1|2|3|4|5] [-g] [--emit-llvm] [--emit-asm] [-S] [--verbose]\n"
                      << "  -O0      true O0: no runtime bitcode LTO, no LLVM passes (debug/IR inspect)\n"
                      << "  -O1      O1 + runtime LTO (default path for light opt)\n"
                      << "  -O2      O2 + runtime LTO (default)\n"
                      << "  -O3      O3 + runtime LTO\n"
                      << "  -O4      O3 + ThinLTO + PGO + aggressive loop opts + target-specific\n"
                      << "  -O5      O3 + Full LTO + multi-versioning + extreme loop opts\n"
                      << "  -g       emit DWARF debug info (source line mapping for gdb/lldb)\n"
                      << "  --pgo-instrument  generate instrumentation for PGO (O4+)\n"
                      << "  --pgo-use=FILE    use profile FILE for PGO (O4+)\n"
                      << "  --pgo-generate-profile[=SCRIPT]  compile instrumented, run SCRIPT, merge .profraw → .profdata (O4+)\n"
                      << "  -mcpu=NAME        target CPU for codegen (O4+)\n"
                      << "  -march=NAME       target architecture for codegen (O4+)\n"
                      << "  --target-arch=NAME  multi-versioning target CPU (sandybridge, nehalem, znver3, etc.)\n"
                      << "  --opt=N  alias for -ON (deprecated)\n"
                      << "  --venv [PATH]  use virtual environment (optional path)\n"
                      << "  --python[=path]  specify Python installation path (auto-detects version)\n"
                      << "  --pystdlib PATH  specify Python standard library path (deprecated, use --python)\n"
                      << "  --dynamic-link  link modules dynamically instead of statically\n";
            return 0;
        }
    }
    
    if (argc < 2) {
        std::cerr << "Usage: pyc <input.py> [-o output] [--static] [-O0|1|2|3|4|5] [-g] [--emit-llvm] [--emit-asm] [-S] [--verbose]\n"
                  << "  -O0      true O0: no runtime bitcode LTO, no LLVM passes (debug/IR inspect)\n"
                  << "  -O1      O1 + runtime LTO (default path for light opt)\n"
                  << "  -O2      O2 + runtime LTO (default)\n"
                  << "  -O3      O3 + runtime LTO\n"
                  << "  -O4      O3 + ThinLTO + PGO + aggressive loop opts + target-specific\n"
                  << "  -O5      O3 + Full LTO + multi-versioning + extreme loop opts\n"
                  << "  -g       emit DWARF debug info (source line mapping for gdb/lldb)\n"
                  << "  --pgo-instrument  generate instrumentation for PGO (O4+)\n"
                  << "  --pgo-use=FILE    use profile FILE for PGO (O4+)\n"
                  << "  --pgo-generate-profile[=SCRIPT]  compile instrumented, run SCRIPT, merge .profraw → .profdata (O4+)\n"
                  << "  -mcpu=NAME        target CPU for codegen (O4+)\n"
                  << "  -march=NAME       target architecture for codegen (O4+)\n"
                   << "  --target-arch=NAME  multi-versioning target CPU (sandybridge, nehalem, znver3, etc.)\n"
                   << "  --opt=N  alias for -ON (deprecated)\n"
                   << "  --venv [PATH]  use virtual environment (optional path)\n"
                   << "  --python[=path]  specify Python installation path (auto-detects version)\n"
                   << "  --pystdlib PATH  specify Python standard library path (deprecated, use --python)\n"
                   << "  --dynamic-link  link modules dynamically instead of statically\n";
        return 1;
    }
    std::string input;
    std::string output = "a.out";
    bool useStatic = false;
    int optLevel = 2;
    bool emitLLVM = false;
    bool emitASM = false;
    bool verbose = false;
    bool debugInfo = false;
    std::string pgoInstrument;
    std::string pgoProfile;
    std::string pgoGenerateProfile;
    std::string mcpu;
    std::string march;
    std::string targetArch;
    std::string venvPath;
    std::string pythonPath;
    std::string pystdlibPath;
    bool dynamicLink = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) output = argv[++i];
        else if (arg == "--static") useStatic = true;
        else if (arg.rfind("-O", 0) == 0 && arg.size() > 2) optLevel = std::stoi(arg.substr(2));
        else if (arg.rfind("--opt=", 0) == 0) optLevel = std::stoi(arg.substr(6));
        else if (arg == "--emit-llvm") emitLLVM = true;
        else if (arg == "--emit-asm" || arg == "-S") emitASM = true;
        else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        }
        else if (arg == "-g") debugInfo = true;
        else if (arg == "--pgo-instrument") pgoInstrument = "instrument";
        else if (arg.rfind("--pgo-use=", 0) == 0) pgoProfile = arg.substr(10);
        else if (arg.rfind("--pgo-generate-profile", 0) == 0) {
            if (arg.size() > 23) pgoGenerateProfile = arg.substr(23);
            else pgoGenerateProfile = "__auto__";
        }
        else if (arg.rfind("-mcpu=", 0) == 0) mcpu = arg.substr(6);
        else if (arg.rfind("-march=", 0) == 0) march = arg.substr(7);
        else if (arg.rfind("--target-arch=", 0) == 0) targetArch = arg.substr(14);
        else if (arg == "--dynamic-link") dynamicLink = true;
        else if (arg.rfind("--venv=", 0) == 0) venvPath = arg.substr(7);
        else if (arg == "--venv" && i + 1 < argc) venvPath = argv[++i];
        else if (arg.rfind("--python=", 0) == 0) pythonPath = arg.substr(9);
        else if (arg == "--python" && i + 1 < argc) pythonPath = argv[++i];
        else if (arg.rfind("--pystdlib=", 0) == 0) pystdlibPath = arg.substr(11);
        else if (arg == "--pystdlib" && i + 1 < argc) pystdlibPath = argv[++i];
        else if (input.empty()) input = arg;
    }
    pyc::Compiler c;
    if (c.compile(input, output, useStatic, optLevel, emitLLVM, emitASM, verbose, debugInfo,
                  pgoInstrument, pgoProfile, pgoGenerateProfile, mcpu, march, targetArch, venvPath, dynamicLink, pythonPath)) return 0;
    return 1;
}
