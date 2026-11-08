#pragma once
// The lowering entry point shared by the two front doors of the single pyc
// binary:
//   * the driver (compiler/src/driver.cpp) lowers the envelope in-process, then
//     links; and
//   * the `--lower` mode (compiler/src/pyc_lower_main.cpp), the former
//     standalone `pyc_lower` binary, which reads an envelope and prints IR so
//     the IR-level checks (check_ir_wellformed.py, coverage.py) can drive the
//     lowering directly without a sysroot or a link.
//
// This is why the driver and the backend live in ONE binary now: the backend
// is a plain function, not a separate process the driver has to find and launch.
#include <string>

namespace pyc {
namespace ir {
struct Module;
}

// Read a pyc_parse JSON envelope (the text of the file or stdin), deserialize
// the AST, and lower it into `m`. Diagnostics go to stderr in the
// file:line:col: kind form (INTERFACES §1.1). Returns 0 on success, nonzero on
// a parse/envelope/lowering failure.
int lower_envelope(const std::string& json_text, const std::string& default_file,
                   ir::Module& m);

// Backend: emit the LLVM IR for the lowered module (the driver links this).
std::string codegen_llvm(const ir::Module& m);

}  // namespace pyc
