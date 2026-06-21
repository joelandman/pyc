// main.cpp - PyC compiler driver
// Orchestrates the full pipeline: source → lexer → parser → AST → IR → LLVM IR

#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <memory>
#include <cstdlib>

#include "frontend/lexer.h"
#include "frontend/ast.h"
#include "frontend/parser.h"
#include "ir/ir.h"
#include "ir/builder.h"
#include "codegen/ir2ll.h"

namespace fs = std::filesystem;

// ===== Utility Functions =====

void print_usage(const char* program) {
    std::cout << "PyC - Python 3 Compiler (LLVM Backend)\n\n"
              << "Usage: " << program << " [OPTIONS] <source.py>\n\n"
              << "Options:\n"
              << "  -o <file>     Output binary file\n"
              << "  --emit-ir     Print IR after generation\n"
              << "  --emit-llir   Print LLVM IR after generation\n"
              << "  --test <name> Run test suite (lexer|ir|codegen|full)\n"
              << "  --test-compile <test_dir> Run compile tests\n"
              << "  --test-ir <source.py> Validate IR generation\n"
              << "  --help        Show this message\n"
              << "  -v            Verbose output\n"
              << std::endl;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// ===== Full Compiler Pipeline =====

void compiler_pipeline(const std::string& source_path, bool verbose, bool emit_llvm_ir) {
    std::cout << "PyC compiler v0.1\n";
    std::cout << "======================\n";

    // Phase 1: Read source file
    std::cout << "\n[Phase 1] Reading source: " << source_path << "\n";
    auto source = read_file(source_path);
    std::cout << "  Read " << source.size() << " bytes ("
              << source.size() / 80 << " lines)\n";

    // Phase 2: Lexical analysis (using legacy lexer)
    std::cout << "\n[Phase 2] Lexical analysis...\n";
    auto tokens = pyc::lexer::tokenize(source);
    std::cout << "  Tokenized " << tokens.size() << " tokens\n";
    if (verbose) {
        for (auto& tok : tokens) {
            std::cout << "  Token: " << static_cast<int>(tok.kind) << " = " << tok.value << "\n";
        }
    }

    // Phase 3: Parse (using recursive descent parser)
    std::cout << "\n[Phase 3] Parsing...\n";
    pyc::parser::Parser parser(tokens);
    auto ast_module = parser.parse();
    std::cout << "  Parsed AST with " << ast_module->body().size() << " statements\n";

    // Phase 4: Classify classes/types in AST
    std::cout << "\n[Phase 4] Type/classification...\n";
    ast_module->classify_funcs_and_classes();
    std::cout << "  Found " << ast_module->functions().size() << " functions\n";

    // Phase 5: Generate IR
    std::cout << "\n[Phase 5] IR generation...\n";
    pyc::ir::builder::IRBuilder ir_builder;
    ir_builder.build(*ast_module);
    int func_count = static_cast<int>(ir_builder.module->functions.size());
    int block_count = 0;
    for (auto& [name, fn] : ir_builder.module->functions) {
        (void)name;
        block_count += static_cast<int>(fn->blocks.size());
    }
    std::cout << "  Generated " << func_count << " functions, "
              << block_count << " blocks\n";

    if (emit_llvm_ir) {
        std::cout << "\n[Phase 6] LLVM IR output...\n";
        
        pyc::ir::IRModule& ir_mod = *ir_builder.module;
        std::string llir = pyc::codegen::translate_module(ir_mod);
        std::cout << "<<LLVM_IR_OUTPUT>>\n";
        std::cout << llir;
        std::cout << "<<LLVM_IR_OUTPUT_END>>\n";
    }

    // Final step
    std::cout << "\nCompilation complete!\n";
    std::cout << "Use --emit-llvm-ir to see the generated LLVM IR\n";
}

// ===== Test Suites =====

void run_test_suite(const std::string& suite_name) {
    std::cout << "Running " << suite_name << " test suite\n";
    std::cout << "=========================================\n";

    bool success = true;

    if (suite_name == "lexer" || suite_name == "full") {
        std::cout << "\n[Test: Lexer]\n";
        std::string test_code = R"("x = 1 + 2\ny = 'hello world'\n", result = x + len(y))";
        try {
            auto tokens = pyc::lexer::tokenize(test_code);
            std::cout << "  Lexer passed: " << tokens.size() << " tokens\n";
        } catch (const std::exception& e) {
            std::cout << "  Lexer failed: " << e.what();
            success = false;
        }
    }

    if (suite_name == "parser" || suite_name == "full") {
        std::cout << "\n[Test: Parser]\n";
        std::string test_code = "def add(x, y):\n    return x + y\na = 1 + 2\nb = 'hello'\n";
        try {
            auto tokens = pyc::lexer::tokenize(test_code);
            pyc::parser::Parser parser(tokens);
            auto mod = parser.parse();
            std::cout << "  Parser passed: " << mod->body().size() << " statements\n";
        } catch (const std::exception& e) {
            std::cout << "  Parser failed: " << e.what();
            success = false;
        }
    }

    if (suite_name == "ir" || suite_name == "full") {
        std::cout << "\n[Test: IR]\n";
        std::string test_code = "def add(x, y):\n    return x + y\n";
        try {
            auto tokens = pyc::lexer::tokenize(test_code);
            pyc::parser::Parser parser(tokens);
            auto mod = parser.parse();
            pyc::ir::builder::IRBuilder builder;
            builder.build(*mod);
            auto func_count = builder.module->functions.size();
            std::cout << "  IR passed: " << func_count << " functions\n";
        } catch (const std::exception& e) {
            std::cout << "  IR failed: " << e.what();
            success = false;
        }
    }

     if (suite_name == "codegen" || suite_name == "full") {
        std::cout << "\n[Test: Codegen (LLVM IR)\n";
        std::string test_code = "def main():\n    x = 10\n    y = 20\n    z = x + y\n";
        try {
            std::cout << "  Parsing code...\n";
            auto tokens = pyc::lexer::tokenize(test_code);
            std::cout << "  Tokens: " << tokens.size() << "\n";
            for (size_t i = 0; i < tokens.size(); i++) {
                std::cout << "    " << i << ": kind=" << static_cast<int>(tokens[i].kind) << " value='" << tokens[i].value << "'\n";
            }
            std::cout << "  Creating parser...\n";
            pyc::parser::Parser parser(tokens);
            std::cout << "  Parsing...\n";
            auto mod = parser.parse();
            std::cout << "  AST: " << mod->body().size() << " statements\n";
            for (auto& stmt : mod->body()) {
                std::cout << "    Stmt: " << typeid(*stmt).name() << "\n";
            }
            std::cout << "  Building IR...\n";
            pyc::ir::builder::IRBuilder builder;
            builder.build(*mod);
            std::cout << "  IR built successfully\n";
            
            // Debug: print IR
            std::cout << "  Debug: IR functions = " << builder.module->functions.size() << "\n";
            for (auto& [name, fn] : builder.module->functions) {
                std::cout << "  Debug: function " << name << " has " << fn->blocks.size() << " blocks\n";
                for (size_t i = 0; i < fn->blocks.size(); i++) {
                    std::cout << "    Block " << i << ": " << fn->blocks[i]->instrs.size() << " instrs\n";
                    for (auto& instr : fn->blocks[i]->instrs) {
                        std::cout << "      Instr: kind=" << static_cast<int>(instr->kind) << " name=" << instr->name << "\n";
                    }
                }
            }
            
            std::cout << "  Running codegen...\n";
            auto llir = pyc::codegen::translate_module(*builder.module);
            if (!llir.empty()) {
                std::cout << "\n  Codegen passed! Generated LLVM IR\n";
                std::cout << "\n(Truncated output - " << llir.length() << " bytes)\n";
                // Print first 20 lines of LLVM IR
                size_t pos = 0, line_count = 0;
                while (pos < llir.size() && line_count < 20) {
                    size_t next = llir.find('\n', pos);
                    if (next == std::string::npos) next = llir.size();
                    std::cout << "  " << llir.substr(pos, next - pos) << "\n";
                    pos = next + 1;
                    line_count++;
                }
            } else {
                std::cout << "  Codegen: no LLVM IR generated\n";
                success = false;
            }
        } catch (const std::exception& e) {
            std::cout << "  Codegen failed: " << e.what();
            success = false;
        }
    }

    std::cout << "\n" << (success ? "All tests PASSED!" : "Some tests FAILED!\n");
}

// ===== Test Compile Runner =====

void run_test_compile(const std::string& test_dir) {
    std::cout << "Running compile tests from: " << test_dir << "\n";
    std::cout << "=========================================\n";
    std::cout << "Note: Parser expects JSON input from lark_bridge.py\n";
    std::cout << "Running lexer tests only...\n";
    
    std::vector<std::string> test_cases = {
        "x = 1 + 2",
        "def add(a, b):\n    return a + b",
        "if x > 0:\n    print(x)"
    };
    
    int passed = 0;
    int failed = 0;
    
    for (size_t i = 0; i < test_cases.size(); ++i) {
        std::cout << "\n[Test " << (i + 1) << "/" << test_cases.size() << "]\n";
        try {
            auto tokens = pyc::lexer::tokenize(test_cases[i]);
            std::cout << "  Lexer passed: " << tokens.size() << " tokens\n";
            passed++;
        } catch (const std::exception& e) {
            std::cout << "  Lexer failed: " << e.what() << "\n";
            failed++;
        }
    }
    
    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
}

// ===== Test IR Runner =====

void run_test_ir(const std::string& source_path) {
    std::cout << "Testing IR generation for: " << source_path << "\n";
    std::cout << "=========================================\n";
    
    try {
        std::string source = read_file(source_path);
        
        // Tokenize
        auto tokens = pyc::lexer::tokenize(source);
        std::cout << "  Tokenized " << tokens.size() << " tokens\n";
        
        // Parse
        pyc::parser::Parser parser(tokens);
        auto mod = parser.parse();
        std::cout << "  Parsed " << mod->body().size() << " statements\n";
        
        // Build IR
        pyc::ir::builder::IRBuilder builder;
        builder.build(*mod);
        std::cout << "  Generated " << builder.module->functions.size() << " functions\n";
        
        // Print IR
        std::cout << "\nGenerated IR:\n";
        for (auto& [name, fn] : builder.module->functions) {
            std::cout << "  Function: " << name << "\n";
            for (auto* blk : fn->blocks) {
                std::cout << "    Block: " << blk->name << "\n";
                for (auto& instr : blk->instrs) {
                    std::cout << "      " << static_cast<int>(instr->kind);
                    if (!instr->name.empty()) std::cout << " (" << instr->name << ")";
                    std::cout << "\n";
                }
            }
        }
        
        std::cout << "\nIR test PASSED!\n";
    } catch (const std::exception& e) {
        std::cout << "IR test FAILED: " << e.what() << "\n";
    }
}

// ===== Command-line argument parsing =====

struct Args {
    std::string source_file;
    std::string output_file;
    std::string test_suite;
    std::string test_compile_dir;
    std::string test_ir_file;
    bool emit_llvm_ir = false;
    bool verbose = false;
};

Args parse_args(int argc, char* argv[]) {
    Args args;
    for (auto i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            print_usage(argv[0]);
            exit(0);
        } else if (arg == "-v") {
            args.verbose = true;
        } else if (arg == "--test") {
            args.test_suite = argv[++i];
        } else if (arg == "--test-compile") {
            args.test_compile_dir = argv[++i];
        } else if (arg == "--test-ir") {
            args.test_ir_file = argv[++i];
        } else if (arg == "-o") {
            args.output_file = argv[++i];
        } else if (arg == "--emit-llvm-ir") {
            args.emit_llvm_ir = true;
        } else if (arg[0] != '-') {
            args.source_file = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            exit(1);
        }
    }
    return args;
}

// ===== Program entry point =====

  int main(int argc, char* argv[]) {
    auto args = parse_args(argc, argv);

    if (!args.test_suite.empty()) {
        run_test_suite(args.test_suite);
        return 0;
    }

    if (!args.test_compile_dir.empty()) {
        run_test_compile(args.test_compile_dir);
        return 0;
    }

    if (!args.test_ir_file.empty()) {
        run_test_ir(args.test_ir_file);
        return 0;
    }

    if (args.source_file.empty()) {
        std::cerr << "Error: no source file specified\n";
        print_usage(argv[0]);
        return 1;
    }

    try {
        compiler_pipeline(args.source_file, args.verbose, args.emit_llvm_ir);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Compiler error: " << e.what() << "\n";
        return 2;
    }
}
