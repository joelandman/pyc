#include "pyc/Compiler.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: pyc <input.py> [-o output] [--static] [-O0|1|2|3] [-g] [--emit-llvm] [--emit-asm] [-S] [--verbose]\n"
                  << "  -O0      true O0: no runtime bitcode LTO, no LLVM passes (debug/IR inspect)\n"
                  << "  -O1      O1 + runtime LTO (default path for light opt)\n"
                  << "  -O2      O2 + runtime LTO (default)\n"
                  << "  -O3      O3 + runtime LTO\n"
                  << "  -g       emit DWARF debug info (source line mapping for gdb/lldb)\n"
                  << "  --opt=N  alias for -ON (deprecated)\n";
        return 1;
    }
    std::string input = argv[1];
    std::string output = "a.out";
    bool useStatic = false;
    int optLevel = 2;
    bool emitLLVM = false;
    bool emitASM = false;
    bool verbose = false;
    bool debugInfo = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) output = argv[++i];
        else if (arg == "--static") useStatic = true;
        else if (arg.rfind("-O", 0) == 0 && arg.size() > 2) optLevel = std::stoi(arg.substr(2));
        else if (arg.rfind("--opt=", 0) == 0) optLevel = std::stoi(arg.substr(6));
        else if (arg == "--emit-llvm") emitLLVM = true;
        else if (arg == "--emit-asm" || arg == "-S") emitASM = true;
        else if (arg == "--verbose") verbose = true;
        else if (arg == "-g") debugInfo = true;
    }
    pyc::Compiler c;
    if (c.compile(input, output, useStatic, optLevel, emitLLVM, emitASM, verbose, debugInfo)) return 0;
    return 1;
}
