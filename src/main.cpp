#include "pyc/Compiler.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: pyc <input.py> [-o output] [--static] [--opt=0|1|2|3]\n";
        return 1;
    }
    std::string input = argv[1];
    std::string output = "a.out";
    bool useStatic = false;
    int optLevel = 2;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) output = argv[++i];
        else if (arg == "--static") useStatic = true;
        else if (arg.rfind("--opt=", 0) == 0) optLevel = std::stoi(arg.substr(6));
    }
    pyc::Compiler c;
    if (c.compile(input, output, useStatic, optLevel)) return 0;
    return 1;
}
