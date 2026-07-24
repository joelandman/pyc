#pragma once
#include <string>

namespace pyc {

class Compiler {
public:
    bool compile(const std::string& inputPath, const std::string& outputPath, bool useStatic = false, int optLevel = 2, bool emitLLVM = false, bool emitASM = false, bool verbose = false, bool debugInfo = false,
                  const std::string& pgoInstrument = "",
                  const std::string& pgoProfile = "",
                  const std::string& pgoGenerateProfile = "",
                  const std::string& mcpu = "",
                  const std::string& march = "",
                  const std::string& targetArch = "");
};

} // namespace pyc
