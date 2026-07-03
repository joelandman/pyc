// ast.cpp - Implementation for AST nodes
// Implements classify_funcs_and_classes() for Module

#include "frontend/ast.h"

namespace pyc::ast {

void Module::classify_funcs_and_classes() {
    functions_.clear();
    classes_.clear();
    for (auto& stmt : body_) {
        if (auto* fd = dynamic_cast<FunctionDef*>(stmt.get())) {
            functions_.push_back(std::static_pointer_cast<FunctionDef>(stmt));
        }
        if (auto* cd = dynamic_cast<ClassDef*>(stmt.get())) {
            classes_.push_back(std::static_pointer_cast<ClassDef>(stmt));
        }
    }
}

} // namespace pyc::ast
