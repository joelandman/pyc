// codegen/ir2ll.cpp - IR to LLVM IR translation pass
// Converts internal IR representation to actual LLVM IR using LLVM C++ API

#include "codegen/ir2ll.h"
#include "ir/ir.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ADT/ArrayRef.h>

#include <string>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pyc::codegen {

static llvm::PointerType* get_i8_ptr(llvm::LLVMContext& ctx) {
    return llvm::PointerType::get(llvm::Type::getInt8Ty(ctx), 0);
}

static llvm::Type* get_i8(llvm::LLVMContext& ctx) {
    return llvm::Type::getInt8Ty(ctx);
}

namespace detail {

class IR2LLVMPass {
    llvm::LLVMContext ctx_;
    std::unique_ptr<llvm::Module> mod_;
    llvm::IRBuilder<> builder_;
    pyc::ir::IRModule& ir_mod_;

    std::unordered_map<uint32_t, llvm::Value*> value_map_;
    std::unordered_map<std::string, llvm::Function*> func_map_;
    std::unordered_map<std::string, llvm::AllocaInst*> param_map_;
    std::string cur_fn_;
    std::unordered_map<uint32_t, llvm::Value*> type_cache_;

public:
    explicit IR2LLVMPass(pyc::ir::IRModule& m)
        : ctx_(), mod_(std::make_unique<llvm::Module>("pyc_module", ctx_)),
          builder_(ctx_), ir_mod_(m) {
        declare_runtime_functions();
    }

    std::string run() {
        for (auto* f : ir_mod_.func_list)
            translate_fn(*f);

        if (!ir_mod_.has_main)
            gen_main_entry();

        std::string out;
        llvm::raw_string_ostream ss(out);
        mod_->print(ss, nullptr);
        return out;
    }

    llvm::Module* get_module() { return mod_.get(); }

private:
    void declare_runtime_functions() {
        auto* obj_type = get_i8_ptr(ctx_);
        auto* i32_type = llvm::Type::getInt32Ty(ctx_);
        auto* i64_type = llvm::Type::getInt64Ty(ctx_);

        llvm::FunctionType::get(obj_type, {i32_type}, false),
        llvm::Function::Create(
            llvm::FunctionType::get(obj_type, {i32_type}, false),
            llvm::Function::ExternalLinkage, "pyc_new_object", mod_.get());

        llvm::Function::Create(
            llvm::FunctionType::get(obj_type, {}, false),
            llvm::Function::ExternalLinkage, "pyc_new_list", mod_.get());

        std::vector<llvm::Type*> list_get_params = {obj_type, i64_type};
        llvm::Function::Create(
            llvm::FunctionType::get(obj_type, list_get_params, false),
            llvm::Function::ExternalLinkage, "pyc_list_get", mod_.get());

        std::vector<llvm::Type*> list_set_params = {obj_type, i64_type, obj_type};
        llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), list_set_params, false),
            llvm::Function::ExternalLinkage, "pyc_list_set", mod_.get());

        std::vector<llvm::Type*> print_params = {obj_type};
        llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), print_params, false),
            llvm::Function::ExternalLinkage, "pyc_print", mod_.get());

        std::vector<llvm::Type*> str_val_params = {obj_type};
        llvm::Function::Create(
            llvm::FunctionType::get(get_i8_ptr(ctx_), str_val_params, false),
            llvm::Function::ExternalLinkage, "pyc_str_value", mod_.get());

        std::vector<llvm::Type*> getattr_params = {obj_type, get_i8_ptr(ctx_)};
        llvm::Function::Create(
            llvm::FunctionType::get(obj_type, getattr_params, false),
            llvm::Function::ExternalLinkage, "pyc_getattr", mod_.get());

        std::vector<llvm::Type*> setattr_params = {obj_type, get_i8_ptr(ctx_), obj_type};
        llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), setattr_params, false),
            llvm::Function::ExternalLinkage, "pyc_setattr", mod_.get());

        std::vector<llvm::Type*> pow_params = {i64_type, i64_type};
        llvm::Function::Create(
            llvm::FunctionType::get(builder_.getDoubleTy(), pow_params, false),
            llvm::Function::ExternalLinkage, "pyc_pow", mod_.get());

        std::vector<llvm::Type*> int_from_dbl_params = {builder_.getDoubleTy()};
        llvm::Function::Create(
            llvm::FunctionType::get(i64_type, int_from_dbl_params, false),
            llvm::Function::ExternalLinkage, "pyc_int_from_double", mod_.get());
    }

    llvm::Value* val(uint32_t id) const {
        auto it = value_map_.find(id);
        return it != value_map_.end() ? it->second : nullptr;
    }

    llvm::Value* get_type_obj(uint32_t type_kind) {
        auto it = type_cache_.find(type_kind);
        if (it != type_cache_.end()) return it->second;
        
        auto* type_gv = new llvm::GlobalVariable(
            *mod_, get_i8_ptr(ctx_),
            false, llvm::GlobalValue::ExternalLinkage,
            nullptr, "pyc_type_" + std::to_string(type_kind));
        
        it = type_cache_.insert({type_kind, type_gv}).first;
        return it->second;
    }

    void translate_fn(pyc::ir::IRFunction& f) {
        cur_fn_ = f.name;
        value_map_.clear();
        param_map_.clear();

        std::vector<llvm::Type*> params;
        for (auto& name : f.param_names)
            params.push_back(llvm::Type::getInt64Ty(ctx_));

        auto* ft = llvm::FunctionType::get(builder_.getInt64Ty(), params, false);
        auto* llvm_fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                                f.name, mod_.get());
        func_map_[f.name] = llvm_fn;

        std::unordered_map<uint32_t, llvm::BasicBlock*> blk_map;
        for (auto* ib : f.blocks) {
            std::string name = ib->name.empty() ? (f.name + ".bb" + std::to_string(ib->id))
                                                 : (f.name + "." + ib->name);
            blk_map[ib->id] = llvm::BasicBlock::Create(ctx_, name, llvm_fn);
        }

        builder_.SetInsertPoint(blk_map[f.entry_block_id]);

        auto arg_iter = llvm_fn->arg_begin();
        for (size_t i = 0; i < f.param_names.size(); ++i, ++arg_iter) {
            auto* alloc = builder_.CreateAlloca(llvm::Type::getInt64Ty(ctx_), nullptr,
                                                 ("param." + f.param_names[i]).c_str());
            builder_.CreateStore(&*arg_iter, alloc);
            param_map_[f.param_names[i]] = alloc;
        }

        for (auto* ib : f.blocks) {
            builder_.SetInsertPoint(blk_map[ib->id]);
            translate_block(*ib, f, blk_map);
        }

        {
            auto* last = blk_map[f.blocks.back()->id];
            if (!last->getTerminator()) {
                builder_.SetInsertPoint(last);
                if (f.name == "main")
                    builder_.CreateRet(builder_.getInt32(0));
                else
                    builder_.CreateRet(builder_.getInt64(0));
            }
        }
    }

    void translate_block(pyc::ir::IRBlock& blk, pyc::ir::IRFunction& fn,
                          std::unordered_map<uint32_t, llvm::BasicBlock*>& blk_map) {
        for (auto& pins : blk.instrs) {
            auto* inst = pins.get();
            auto kind = inst->kind;
            llvm::Value* res = nullptr;
            auto record = [&](llvm::Value* v) {
                res = v;
                if (v) value_map_[inst->id] = v;
            };
            auto i64 = [&]() { return builder_.getInt64(0); };

            switch (kind) {
            case pyc::ir::IRInstKind::LOADCONST_INT:
                if (inst->is_const) {
                    auto dbl = std::get<double>(inst->const_val);
                    record(builder_.CreateFPToSI(
                        llvm::ConstantFP::get(builder_.getDoubleTy(), dbl),
                        builder_.getInt64Ty(), "const_int"));
                }
                break;

            case pyc::ir::IRInstKind::LOADCONST_FLOAT:
                if (inst->is_const) {
                    record(llvm::ConstantFP::get(builder_.getDoubleTy(),
                        std::get<double>(inst->const_val)));
                }
                break;

            case pyc::ir::IRInstKind::LOADCONST_STR:
                if (inst->is_const && std::holds_alternative<std::string>(inst->const_val)) {
                    auto& str_val = std::get<std::string>(inst->const_val);
                    auto* gv = new llvm::GlobalVariable(
                        *mod_, llvm::ArrayType::get(get_i8(ctx_), str_val.size() + 1),
                        true, llvm::GlobalValue::PrivateLinkage,
                        llvm::ConstantDataArray::getString(builder_.getContext(), str_val),
                        "str.const");
                    record(gv);
                }
                break;

            case pyc::ir::IRInstKind::LOADLOCAL:
            case pyc::ir::IRInstKind::LOADGLOBAL: {
                auto it = param_map_.find(inst->name);
                if (it != param_map_.end() && it->second) {
                    record(builder_.CreateLoad(llvm::Type::getInt64Ty(ctx_), it->second,
                                                ("ld." + inst->name).c_str()));
                } else {
                    auto vit = value_map_.find(inst->id);
                    record(vit != value_map_.end() ? vit->second : i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::STORELOCAL:
            case pyc::ir::IRInstKind::STOREGLOBAL: {
                if (inst->operands.size() >= 2) {
                    auto* ptr = builder_.CreateAlloca(llvm::Type::getInt64Ty(ctx_), nullptr,
                                                       (inst->name + ".loc").c_str());
                    auto* v = val(inst->operands[1]);
                    builder_.CreateStore(v ? v : i64(), ptr);
                    param_map_[inst->name] = ptr;
                }
                break;
            }

            case pyc::ir::IRInstKind::ALLOC: {
                auto* ptr = builder_.CreateAlloca(llvm::Type::getInt64Ty(ctx_), nullptr,
                                                   inst->name.empty() ? ".loc" : inst->name.c_str());
                record(ptr);
                break;
            }

            case pyc::ir::IRInstKind::MAKE_LIST: {
                auto* list_fn = mod_->getFunction("pyc_new_list");
                if (list_fn) {
                    std::vector<llvm::Value*> empty_args;
                    record(builder_.CreateCall(list_fn, empty_args, "new_list"));
                } else {
                    record(builder_.getInt64(0));
                }
                break;
            }

            case pyc::ir::IRInstKind::LIST_GET: {
                if (inst->operands.size() >= 2) {
                    auto* list_fn = mod_->getFunction("pyc_list_get");
                    if (list_fn) {
                        std::vector<llvm::Value*> args = {
                            val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                            val(inst->operands[1]) ? val(inst->operands[1]) : i64()
                        };
                        record(builder_.CreateCall(list_fn, args, "list_get"));
                    } else {
                        record(i64());
                    }
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::LIST_SET: {
                if (inst->operands.size() >= 3) {
                    auto* list_fn = mod_->getFunction("pyc_list_set");
                    if (list_fn) {
                        std::vector<llvm::Value*> args = {
                            val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                            val(inst->operands[1]) ? val(inst->operands[1]) : i64(),
                            val(inst->operands[2]) ? val(inst->operands[2]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                        };
                        builder_.CreateCall(list_fn, args);
                    }
                }
                break;
            }

            case pyc::ir::IRInstKind::NEWOBJ: {
                auto* new_obj_fn = mod_->getFunction("pyc_new_object");
                if (new_obj_fn) {
                    auto* type_val = get_type_obj(inst->operands.empty() ? 0 : inst->operands[0]);
                    std::vector<llvm::Value*> args = {type_val};
                    record(builder_.CreateCall(new_obj_fn, args, "new_obj"));
                } else {
                    record(builder_.getInt64(0));
                }
                break;
            }

            case pyc::ir::IRInstKind::INTRINSIC_PRINT: {
                auto* print_fn = mod_->getFunction("pyc_print");
                if (print_fn && inst->operands.size() >= 1) {
                    std::vector<llvm::Value*> args = {
                        val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                    };
                    record(builder_.CreateCall(print_fn, args, "print_call"));
                }
                break;
            }

            case pyc::ir::IRInstKind::INTRINSIC_RANGE: {
                auto* list_fn = mod_->getFunction("pyc_new_list");
                if (list_fn) {
                    std::vector<llvm::Value*> empty_args;
                    record(builder_.CreateCall(list_fn, empty_args, "range_list"));
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::INTRINSIC_TYPE:
            case pyc::ir::IRInstKind::INTRINSIC_LEN:
            case pyc::ir::IRInstKind::INTRINSIC_INIT:
                record(i64());
                break;

           case pyc::ir::IRInstKind::GETATTR:
            case pyc::ir::IRInstKind::LOAD_ATTR: {
                if (inst->operands.size() >= 1) {
                    auto* getattr_fn = mod_->getFunction("pyc_getattr");
                    if (getattr_fn) {
                        std::vector<llvm::Value*> args = {
                            val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                        };
                        record(builder_.CreateCall(getattr_fn, args, "getattr"));
                    } else {
                        record(i64());
                    }
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::SETATTR: {
                if (inst->operands.size() >= 2) {
                    auto* setattr_fn = mod_->getFunction("pyc_setattr");
                    if (setattr_fn) {
                        std::vector<llvm::Value*> args = {
                            val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                            llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                            val(inst->operands[1]) ? val(inst->operands[1]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                        };
                        builder_.CreateCall(setattr_fn, args);
                    }
                }
                break;
            }

            case pyc::ir::IRInstKind::CALL: {
                auto fit = func_map_.find(inst->name);
                if (fit != func_map_.end()) {
                    std::vector<llvm::Value*> call_args;
                    for (size_t i = 1; i < inst->operands.size(); ++i) {
                        auto* a = val(inst->operands[i]);
                        call_args.push_back(a ? a : i64());
                    }
                    record(builder_.CreateCall(fit->second, call_args));
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::RETURN: {
                if (inst->operands.size() >= 1) {
                    auto* rv = val(inst->operands[0]);
                    builder_.CreateRet(rv ? rv : i64());
                } else {
                    if (fn.name == "main")
                        builder_.CreateRet(builder_.getInt32(0));
                    else
                        builder_.CreateRet(i64());
                }
                return;
            }

            case pyc::ir::IRInstKind::JUMP:
            case pyc::ir::IRInstKind::LABEL: {
                if (inst->operands.size() >= 1) {
                    auto* t = blk_map[inst->operands[0]];
                    if (t) builder_.CreateBr(t);
                }
                break;
            }

            case pyc::ir::IRInstKind::BRANCH: {
                if (inst->operands.size() >= 3) {
                    auto* cond = val(inst->operands[0]);
                    auto* true_b  = blk_map[inst->operands[1]];
                    auto* false_b = blk_map[inst->operands[2]];
                    if (cond && true_b && false_b) {
                        builder_.CreateCondBr(cond, true_b, false_b);
                    }
                }
                break;
            }

            case pyc::ir::IRInstKind::PHI: {
                auto* phi = builder_.CreatePHI(llvm::Type::getInt64Ty(ctx_), 2,
                                                inst->name.empty() ? "phi" : inst->name.c_str());
                if (!inst->operands.empty()) {
                    auto* v = val(inst->operands[0]);
                    auto* e = blk_map[fn.entry_block_id];
                    if (e) phi->addIncoming(v ? v : i64(), e);
                }
                record(phi);
                break;
            }

            case pyc::ir::IRInstKind::ADD:
                record(builder_.CreateAdd(val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                                           val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "add"));
                break;
            case pyc::ir::IRInstKind::SUB:
                record(builder_.CreateSub(val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                                           val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "sub"));
                break;
            case pyc::ir::IRInstKind::MUL:
                record(builder_.CreateMul(val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                                           val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "mul"));
                break;
            case pyc::ir::IRInstKind::DIV:
                record(builder_.CreateSDiv(val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                                            val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "div"));
                break;
            case pyc::ir::IRInstKind::MOD:
                record(builder_.CreateSRem(val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                                            val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "mod"));
                break;
             case pyc::ir::IRInstKind::POW: {
                if (inst->operands.size() >= 2) {
                    auto* pow_fn = mod_->getFunction("pyc_pow");
                    if (pow_fn) {
                        std::vector<llvm::Value*> args = {
                            val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                            val(inst->operands[1]) ? val(inst->operands[1]) : i64()
                        };
                        auto* pow_call = builder_.CreateCall(pow_fn, args, "pow_call");
                        auto* result = builder_.CreateFPToSI(pow_call, builder_.getInt64Ty(), "pow_result");
                        record(result);
                    } else {
                        auto* base = val(inst->operands[0]) ? val(inst->operands[0]) : i64();
                        auto* exp = val(inst->operands[1]) ? val(inst->operands[1]) : i64();
                        auto* base_dbl = builder_.CreateSIToFP(base, builder_.getDoubleTy(), "base_dbl");
                        auto* exp_dbl = builder_.CreateSIToFP(exp, builder_.getDoubleTy(), "exp_dbl");
                        auto* result_dbl = builder_.CreateCall(builder_.CreateIntrinsic(llvm::Intrinsic::pow, {builder_.getDoubleTy()}, {}), {base_dbl, exp_dbl}, "pow_fallback");
                        record(builder_.CreateFPToSI(result_dbl, builder_.getInt64Ty(), "pow_result"));
                    }
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::LT:
                record(builder_.CreateICmp(llvm::ICmpInst::ICMP_SLT,
                    val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                    val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "lt"));
                break;
            case pyc::ir::IRInstKind::LE:
                record(builder_.CreateICmp(llvm::ICmpInst::ICMP_SLE,
                    val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                    val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "le"));
                break;
            case pyc::ir::IRInstKind::GT:
                record(builder_.CreateICmp(llvm::ICmpInst::ICMP_SGT,
                    val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                    val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "gt"));
                break;
            case pyc::ir::IRInstKind::GE:
                record(builder_.CreateICmp(llvm::ICmpInst::ICMP_SGE,
                    val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                    val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "ge"));
                break;
            case pyc::ir::IRInstKind::EQ:
                record(builder_.CreateICmp(llvm::ICmpInst::ICMP_EQ,
                    val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                    val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "eq"));
                break;
            case pyc::ir::IRInstKind::NE:
                record(builder_.CreateICmp(llvm::ICmpInst::ICMP_NE,
                    val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                    val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "ne"));
                break;

            case pyc::ir::IRInstKind::AND:
                record(builder_.CreateAnd(val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                                           val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "and"));
                break;
            case pyc::ir::IRInstKind::OR:
                record(builder_.CreateOr(val(inst->operands[0]) ? val(inst->operands[0]) : i64(),
                                          val(inst->operands[1]) ? val(inst->operands[1]) : i64(), "or"));
                break;
            case pyc::ir::IRInstKind::NOT:
                record(builder_.CreateNot(val(inst->operands[0]) ? val(inst->operands[0]) : i64(), "not"));
                break;

            case pyc::ir::IRInstKind::ISINSTANCE:
                record(i64());
                break;

            default:
                record(i64());
                break;
            }
        }
    }

    void gen_main_entry() {
        auto* mt = llvm::FunctionType::get(builder_.getInt32Ty(), false);
        auto* mf = llvm::Function::Create(mt, llvm::Function::ExternalLinkage,
                                            "_pyc_main", mod_.get());
        auto* e = llvm::BasicBlock::Create(ctx_, "entry", mf);
        builder_.SetInsertPoint(e);

        auto fit = func_map_.find("main");
        if (fit != func_map_.end())
            builder_.CreateCall(fit->second, {});

        builder_.CreateRet(builder_.getInt32(0));
        ir_mod_.has_main = true;
    }
};

} // namespace detail

std::string translate_module(pyc::ir::IRModule& ir_mod) {
    detail::IR2LLVMPass pass(ir_mod);
    return pass.run();
}

llvm::Module* translate_module_to_llvm(pyc::ir::IRModule& ir_mod, llvm::LLVMContext& ctx) {
    (void)ctx;
    detail::IR2LLVMPass pass(ir_mod);
    return pass.get_module();
}

} // namespace pyc::codegen
