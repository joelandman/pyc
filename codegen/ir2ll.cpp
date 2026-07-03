// codegen/ir2ll.cpp - IR to LLVM IR translation pass
// Converts internal IR representation to actual LLVM IR using LLVM C++ API

#include "codegen/ir2ll.h"
#include "ir/ir.h"
#include <iostream>
#include <fstream>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/IR/PassManager.h>
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
        for (auto* f : ir_mod_.func_list) {
            translate_fn(*f);
        }

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

        // INTRINSIC_TYPE: pyc_type_name(obj) -> obj
        std::vector<llvm::Type*> type_name_params = {obj_type};
        llvm::Function::Create(
            llvm::FunctionType::get(obj_type, type_name_params, false),
            llvm::Function::ExternalLinkage, "pyc_type_name", mod_.get());

        // INTRINSIC_LEN: pyc_len(obj) -> i64
        std::vector<llvm::Type*> len_params = {obj_type};
        llvm::Function::Create(
            llvm::FunctionType::get(i64_type, len_params, false),
            llvm::Function::ExternalLinkage, "pyc_len", mod_.get());

        // INTRINSIC_INIT: pyc_object_init(obj) -> obj
        std::vector<llvm::Type*> init_params = {obj_type};
        llvm::Function::Create(
            llvm::FunctionType::get(obj_type, init_params, false),
            llvm::Function::ExternalLinkage, "pyc_object_init", mod_.get());

        // ISINSTANCE: pyc_isinstance(obj, type_kind) -> i64
        std::vector<llvm::Type*> isinstance_params = {obj_type, i32_type};
        llvm::Function::Create(
            llvm::FunctionType::get(i64_type, isinstance_params, false),
            llvm::Function::ExternalLinkage, "pyc_isinstance", mod_.get());

        // NEWTYPE: pyc_new_type(type_kind) -> obj
        llvm::Function::Create(
            llvm::FunctionType::get(obj_type, {i32_type}, false),
            llvm::Function::ExternalLinkage, "pyc_new_type", mod_.get());

        // refcount helpers
        std::vector<llvm::Type*> ref_inc_params = {obj_type};
        llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), ref_inc_params, false),
            llvm::Function::ExternalLinkage, "pyc_ref_inc", mod_.get());

        llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), ref_inc_params, false),
            llvm::Function::ExternalLinkage, "pyc_ref_dec", mod_.get());

        std::vector<llvm::Type*> int_from_dbl_params = {builder_.getDoubleTy()};
        llvm::Function::Create(
            llvm::FunctionType::get(i64_type, int_from_dbl_params, false),
            llvm::Function::ExternalLinkage, "pyc_int_from_double", mod_.get());

        // Exception handling
        std::vector<llvm::Type*> exc_params = {obj_type};
        llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), exc_params, false),
            llvm::Function::ExternalLinkage, "pyc_raise_exception", mod_.get());

        llvm::Function::Create(
            llvm::FunctionType::get(obj_type, {}, false),
            llvm::Function::ExternalLinkage, "pyc_get_exception", mod_.get());

        llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(ctx_), {}, false),
            llvm::Function::ExternalLinkage, "pyc_clear_exception", mod_.get());

        // Module loading
        std::vector<llvm::Type*> import_params = {get_i8_ptr(ctx_)};
        llvm::Function::Create(
            llvm::FunctionType::get(obj_type, import_params, false),
            llvm::Function::ExternalLinkage, "pyc_import_module", mod_.get());
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
                    double dbl = 0.0;
                    if (std::holds_alternative<int>(inst->const_val))
                        dbl = static_cast<double>(std::get<int>(inst->const_val));
                    else if (std::holds_alternative<double>(inst->const_val))
                        dbl = std::get<double>(inst->const_val);
                    record(builder_.CreateFPToSI(
                        llvm::ConstantFP::get(builder_.getDoubleTy(), dbl),
                        builder_.getInt64Ty(), "const_int"));
                }
                break;

            case pyc::ir::IRInstKind::LOADCONST_FLOAT:
                if (inst->is_const) {
                    double dbl = 0.0;
                    if (std::holds_alternative<int>(inst->const_val))
                        dbl = static_cast<double>(std::get<int>(inst->const_val));
                    else if (std::holds_alternative<double>(inst->const_val))
                        dbl = std::get<double>(inst->const_val);
                    record(llvm::ConstantFP::get(builder_.getDoubleTy(), dbl));
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

            case pyc::ir::IRInstKind::SLICE_GET: {
                if (inst->operands.size() >= 4) {
                    auto* slice_fn = mod_->getFunction("pyc_slice");
                    if (slice_fn) {
                        std::vector<llvm::Value*> args = {
                            val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                            val(inst->operands[1]) ? val(inst->operands[1]) : i64(),
                            val(inst->operands[2]) ? val(inst->operands[2]) : i64(),
                            val(inst->operands[3]) ? val(inst->operands[3]) : i64()
                        };
                        record(builder_.CreateCall(slice_fn, args, "slice"));
                    } else {
                        record(i64());
                    }
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::MAKE_DICT: {
                auto* dict_fn = mod_->getFunction("pyc_new_dict");
                if (dict_fn) {
                    std::vector<llvm::Value*> empty_args;
                    auto* dict_val = builder_.CreateCall(dict_fn, empty_args, "new_dict");
                    // Set key-value pairs
                    for (size_t i = 0; i + 1 < inst->operands.size(); i += 2) {
                        auto* dict_set_fn = mod_->getFunction("pyc_dict_set");
                        if (dict_set_fn) {
                            std::vector<llvm::Value*> args = {
                                dict_val,
                                val(inst->operands[i]) ? val(inst->operands[i]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                                val(inst->operands[i+1]) ? val(inst->operands[i+1]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                            };
                            builder_.CreateCall(dict_set_fn, args);
                        }
                    }
                    record(dict_val);
                } else {
                    record(builder_.getInt64(0));
                }
                break;
            }

            case pyc::ir::IRInstKind::MAKE_TUPLE: {
                auto* tuple_fn = mod_->getFunction("pyc_new_tuple");
                if (tuple_fn) {
                    std::vector<llvm::Value*> empty_args;
                    record(builder_.CreateCall(tuple_fn, empty_args, "new_tuple"));
                } else {
                    record(builder_.getInt64(0));
                }
                break;
            }

            case pyc::ir::IRInstKind::DICT_GET: {
                if (inst->operands.size() >= 2) {
                    auto* dict_get_fn = mod_->getFunction("pyc_dict_get");
                    if (dict_get_fn) {
                        std::vector<llvm::Value*> args = {
                            val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                            val(inst->operands[1]) ? val(inst->operands[1]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                        };
                        record(builder_.CreateCall(dict_get_fn, args, "dict_get"));
                    } else {
                        record(i64());
                    }
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::DICT_SET: {
                if (inst->operands.size() >= 3) {
                    auto* dict_set_fn = mod_->getFunction("pyc_dict_set");
                    if (dict_set_fn) {
                        std::vector<llvm::Value*> args = {
                            val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                            val(inst->operands[1]) ? val(inst->operands[1]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                            val(inst->operands[2]) ? val(inst->operands[2]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                        };
                        builder_.CreateCall(dict_set_fn, args);
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
                auto* range_fn = mod_->getFunction("pyc_range_list");
                if (range_fn && inst->operands.size() >= 3) {
                    int64_t start = 0, stop = 0, step = 1;
                    
                    auto* start_val = val(inst->operands[0]);
                    if (start_val) start = static_cast<int64_t>(llvm::cast<llvm::ConstantInt>(start_val)->getSExtValue());
                    
                    auto* stop_val = val(inst->operands[1]);
                    if (stop_val) stop = static_cast<int64_t>(llvm::cast<llvm::ConstantInt>(stop_val)->getSExtValue());
                    
                    auto* step_val = val(inst->operands[2]);
                    if (step_val) step = static_cast<int64_t>(llvm::cast<llvm::ConstantInt>(step_val)->getSExtValue());
                    
                    auto* i64_type = llvm::Type::getInt64Ty(ctx_);
                    auto* start_c = llvm::ConstantInt::get(i64_type, static_cast<int64_t>(start));
                    auto* stop_c = llvm::ConstantInt::get(i64_type, static_cast<int64_t>(stop));
                    auto* step_c = llvm::ConstantInt::get(i64_type, static_cast<int64_t>(step));
                    llvm::SmallVector<llvm::Value*, 3> range_args;
                    range_args.push_back(start_c);
                    range_args.push_back(stop_c);
                    range_args.push_back(step_c);
                    record(builder_.CreateCall(range_fn, range_args, "range_call"));
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::INTRINSIC_TYPE: {
                auto* type_name_fn = mod_->getFunction("pyc_type_name");
                if (type_name_fn && inst->operands.size() >= 1) {
                    std::vector<llvm::Value*> args = {
                        val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                    };
                    record(builder_.CreateCall(type_name_fn, args, "type_name"));
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::INTRINSIC_LEN: {
                auto* len_fn = mod_->getFunction("pyc_len");
                if (len_fn && inst->operands.size() >= 1) {
                    std::vector<llvm::Value*> args = {
                        val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                    };
                    record(builder_.CreateCall(len_fn, args, "len_call"));
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::INTRINSIC_INIT: {
                auto* init_fn = mod_->getFunction("pyc_object_init");
                if (init_fn && inst->operands.size() >= 1) {
                    std::vector<llvm::Value*> args = {
                        val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                    };
                    record(builder_.CreateCall(init_fn, args, "object_init"));
                } else {
                    record(i64());
                }
                break;
            }

           case pyc::ir::IRInstKind::GETATTR:
            case pyc::ir::IRInstKind::LOAD_ATTR: {
                if (inst->operands.size() >= 1) {
                    auto* getattr_fn = mod_->getFunction("pyc_getattr");
                    if (getattr_fn) {
                        // Create a GlobalVariable for the attribute name string
                        std::string attr_name = inst->name.empty() ? "" : inst->name;
                        auto* attr_gv = new llvm::GlobalVariable(
                            *mod_, llvm::ArrayType::get(get_i8(ctx_), attr_name.size() + 1),
                            true, llvm::GlobalValue::PrivateLinkage,
                            llvm::ConstantDataArray::getString(builder_.getContext(), attr_name),
                            "getattr_attr_name");
                        
                        std::vector<llvm::Value*> args = {
                            val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                            attr_gv
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
                        // Create a GlobalVariable for the attribute name string
                        std::string attr_name = inst->name.empty() ? "" : inst->name;
                        auto* attr_gv = new llvm::GlobalVariable(
                            *mod_, llvm::ArrayType::get(get_i8(ctx_), attr_name.size() + 1),
                            true, llvm::GlobalValue::PrivateLinkage,
                            llvm::ConstantDataArray::getString(builder_.getContext(), attr_name),
                            "setattr_attr_name");
                        
                        std::vector<llvm::Value*> args = {
                            val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                            attr_gv,
                            val(inst->operands[1]) ? val(inst->operands[1]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                        };
                        builder_.CreateCall(setattr_fn, args);
                    }
                }
                break;
            }

            case pyc::ir::IRInstKind::CALL: {
                // Special handling for builtins
                if (inst->name == "range") {
                    auto* range_fn = mod_->getFunction("pyc_range_list");
                    if (range_fn && inst->operands.size() >= 2) {
                        // range(stop) or range(start, stop) or range(start, stop, step)
                        int64_t start = 0, stop = 0, step = 1;
                        
                        if (inst->operands.size() >= 2) {
                            auto* stop_val = val(inst->operands[1]);
                            if (stop_val) stop = static_cast<int64_t>(llvm::cast<llvm::ConstantInt>(stop_val)->getSExtValue());
                        }
                        if (inst->operands.size() >= 3) {
                            auto* start_val = val(inst->operands[2]);
                            if (start_val) start = static_cast<int64_t>(llvm::cast<llvm::ConstantInt>(start_val)->getSExtValue());
                        }
                        if (inst->operands.size() >= 4) {
                            auto* step_val = val(inst->operands[3]);
                            if (step_val) step = static_cast<int64_t>(llvm::cast<llvm::ConstantInt>(step_val)->getSExtValue());
                        }
                        
                        auto* i64_type = llvm::Type::getInt64Ty(ctx_);
                        auto* start_c = llvm::ConstantInt::get(i64_type, static_cast<int64_t>(start));
                        auto* stop_c = llvm::ConstantInt::get(i64_type, static_cast<int64_t>(stop));
                        auto* step_c = llvm::ConstantInt::get(i64_type, static_cast<int64_t>(step));
                        llvm::SmallVector<llvm::Value*, 3> range_args;
                        range_args.push_back(start_c);
                        range_args.push_back(stop_c);
                        range_args.push_back(step_c);
                        record(builder_.CreateCall(range_fn, range_args, "range_call"));
                    } else {
                        record(i64());
                    }
                } else if (inst->name == "print") {
                    auto* print_fn = mod_->getFunction("pyc_print");
                    if (print_fn && inst->operands.size() >= 2) {
                        std::vector<llvm::Value*> call_args;
                        for (size_t i = 1; i < inst->operands.size(); ++i) {
                            auto* a = val(inst->operands[i]);
                            call_args.push_back(a ? a : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)));
                        }
                        record(builder_.CreateCall(print_fn, call_args));
                    } else {
                        record(i64());
                    }
                } else if (inst->name == "pyc_raise_exception") {
                    auto* raise_fn = mod_->getFunction("pyc_raise_exception");
                    if (raise_fn) {
                        llvm::SmallVector<llvm::Value*, 1> exc_args;
                        if (!inst->operands.empty()) {
                            auto* exc_val = val(inst->operands[0]);
                            exc_args.push_back(exc_val ? exc_val : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)));
                        } else {
                            exc_args.push_back(llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)));
                        }
                        builder_.CreateCall(raise_fn, exc_args);
                    }
                } else {
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

    case pyc::ir::IRInstKind::AND: {
                // Short-circuit: if lhs is 0, result is 0; else result is rhs
                auto* lhs = val(inst->operands[0]);
                auto* rhs = val(inst->operands[1]);
                if (lhs && rhs) {
                    auto* and_fn = mod_->getFunction("pyc_and");
                    if (and_fn) {
                        std::vector<llvm::Value*> args = {lhs, rhs};
                        record(builder_.CreateCall(and_fn, args, "and"));
                    } else {
                        record(builder_.CreateAnd(lhs, rhs, "and"));
                    }
                } else {
                    record(i64());
                }
                break;
            }
            case pyc::ir::IRInstKind::OR: {
                // Short-circuit: if lhs is non-zero, result is lhs; else result is rhs
                auto* lhs = val(inst->operands[0]);
                auto* rhs = val(inst->operands[1]);
                if (lhs && rhs) {
                    auto* or_fn = mod_->getFunction("pyc_or");
                    if (or_fn) {
                        std::vector<llvm::Value*> args = {lhs, rhs};
                        record(builder_.CreateCall(or_fn, args, "or"));
                    } else {
                        record(builder_.CreateOr(lhs, rhs, "or"));
                    }
                } else {
                    record(i64());
                }
                break;
            }
              case pyc::ir::IRInstKind::NOT:
                record(builder_.CreateNot(val(inst->operands[0]) ? val(inst->operands[0]) : i64(), "not"));
                break;

            case pyc::ir::IRInstKind::IN: {
                auto* in_fn = mod_->getFunction("pyc_contains");
                if (in_fn && inst->operands.size() >= 2) {
                    std::vector<llvm::Value*> args = {
                        val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                        val(inst->operands[1]) ? val(inst->operands[1]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                    };
                    record(builder_.CreateCall(in_fn, args, "in"));
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::IS: {
                auto* is_fn = mod_->getFunction("pyc_is");
                if (is_fn && inst->operands.size() >= 2) {
                    std::vector<llvm::Value*> args = {
                        val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                        val(inst->operands[1]) ? val(inst->operands[1]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_))
                    };
                    record(builder_.CreateCall(is_fn, args, "is"));
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::ISINSTANCE: {
                auto* isinstance_fn = mod_->getFunction("pyc_isinstance");
                if (isinstance_fn && inst->operands.size() >= 2) {
                    std::vector<llvm::Value*> args = {
                        val(inst->operands[0]) ? val(inst->operands[0]) : llvm::ConstantPointerNull::get(get_i8_ptr(ctx_)),
                        val(inst->operands[1]) ? val(inst->operands[1]) : i64()
                    };
                    record(builder_.CreateCall(isinstance_fn, args, "isinstance_call"));
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::NEWTYPE: {
                auto* new_type_fn = mod_->getFunction("pyc_new_type");
                if (new_type_fn && inst->operands.size() >= 1) {
                    std::vector<llvm::Value*> args = {
                        val(inst->operands[0]) ? val(inst->operands[0]) : i64()
                    };
                    record(builder_.CreateCall(new_type_fn, args, "new_type"));
                } else {
                    record(i64());
                }
                break;
            }

            case pyc::ir::IRInstKind::BINOP:
            case pyc::ir::IRInstKind::CMP: {
                // Generic binary op/comparison - dispatch based on instruction name
                std::string op_name = inst->name;
                if (inst->operands.size() >= 2) {
                    auto* lhs = val(inst->operands[0]);
                    auto* rhs = val(inst->operands[1]);
                    if (!lhs || !rhs) {
                        record(i64());
                        break;
                    }
                    
                    if (op_name == "ADD") {
                        record(builder_.CreateAdd(lhs, rhs, "binop_add"));
                    } else if (op_name == "SUB") {
                        record(builder_.CreateSub(lhs, rhs, "binop_sub"));
                    } else if (op_name == "MUL") {
                        record(builder_.CreateMul(lhs, rhs, "binop_mul"));
                    } else if (op_name == "DIV") {
                        record(builder_.CreateSDiv(lhs, rhs, "binop_div"));
                    } else if (op_name == "MOD") {
                        record(builder_.CreateSRem(lhs, rhs, "binop_mod"));
                    } else if (op_name == "LT") {
                        auto* cmp = builder_.CreateICmpSLT(lhs, rhs, "binop_lt");
                        record(builder_.CreateZExt(cmp, builder_.getInt64Ty(), "cmp_result"));
                    } else if (op_name == "LE") {
                        auto* cmp = builder_.CreateICmpSLE(lhs, rhs, "binop_le");
                        record(builder_.CreateZExt(cmp, builder_.getInt64Ty(), "cmp_result"));
                    } else if (op_name == "GT") {
                        auto* cmp = builder_.CreateICmpSGT(lhs, rhs, "binop_gt");
                        record(builder_.CreateZExt(cmp, builder_.getInt64Ty(), "cmp_result"));
                    } else if (op_name == "GE") {
                        auto* cmp = builder_.CreateICmpSGE(lhs, rhs, "binop_ge");
                        record(builder_.CreateZExt(cmp, builder_.getInt64Ty(), "cmp_result"));
                    } else if (op_name == "EQ") {
                        auto* cmp = builder_.CreateICmpEQ(lhs, rhs, "binop_eq");
                        record(builder_.CreateZExt(cmp, builder_.getInt64Ty(), "cmp_result"));
                    } else if (op_name == "NE") {
                        auto* cmp = builder_.CreateICmpNE(lhs, rhs, "binop_ne");
                        record(builder_.CreateZExt(cmp, builder_.getInt64Ty(), "cmp_result"));
                    } else {
                        record(i64());
                    }
                } else {
                    record(i64());
                }
                break;
            }

            default:
                // Unexpected instruction - return 0 as fallback
                // In production, this should never be reached if the IR builder is correct
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
    auto* mod = pass.get_module();
    
      // Apply optimization passes - disabled due to LLVM 21 InferFunctionAttrsPass crash
    // TODO: Re-enable with proper PassBuilder setup for LLVM 21
    // {
    //     llvm::PassBuilder pb;
    //     llvm::ModuleAnalysisManager mam;
    //     llvm::FunctionAnalysisManager fam;
    //     llvm::CGSCCAnalysisManager cgscc_am;
    //     pb.registerModuleAnalyses(mam);
    //     pb.registerFunctionAnalyses(fam);
    //     pb.registerCGSCCAnalyses(cgscc_am);
    //     llvm::ModulePassManager mpm;
    //     auto pipeline = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O0);
    //     mpm.addPass(std::move(pipeline));
    //     mpm.run(*mod, mam);
    // }
    
    return pass.run();
}

llvm::Module* translate_module_to_llvm(pyc::ir::IRModule& ir_mod, llvm::LLVMContext& ctx) {
    (void)ctx;
    detail::IR2LLVMPass pass(ir_mod);
    return pass.get_module();
}

} // namespace pyc::codegen
