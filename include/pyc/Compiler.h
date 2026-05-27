#pragma once
#include <string>

namespace pyc {

class Compiler {
public:
    bool compile(const std::string& inputPath, const std::string& outputPath, bool useStatic = false, int optLevel = 2);
};

} // namespace pyc
