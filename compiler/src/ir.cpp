#include "pyc/ir/ir.hpp"

#include <sstream>

namespace pyc::ir {

const char* op_name(Op op) {
    switch (op) {
        case Op::ConstInt:    return "const.int";
        case Op::ConstFloat:  return "const.float";
        case Op::ConstStr:    return "const.str";
        case Op::ConstBytes:  return "const.bytes";
        case Op::ConstBool:   return "const.bool";
        case Op::ConstNone:   return "const.none";
        case Op::LoadGlobal:  return "load.global";
        case Op::StoreGlobal: return "store.global";
        case Op::LoadLocal:   return "load.local";
        case Op::StoreLocal:  return "store.local";
        case Op::CallCApi:    return "call.capi";
        case Op::CallObject:  return "call.object";
        case Op::IncRef:      return "incref";
        case Op::DecRef:      return "decref";
        case Op::Br:          return "br";
        case Op::CondBr:      return "condbr";
        case Op::Return:      return "ret";
        case Op::ReturnErr:   return "ret.err";
        case Op::IsTrue:      return "istrue";
        case Op::Is:          return "is";
        case Op::IntNot:      return "int.not";
        case Op::Phi:         return "phi";
        case Op::IterNext:    return "iter.next";
        case Op::ImportModule: return "import";
        case Op::BuildClass:  return "buildclass";
        case Op::Raise:       return "raise";
        case Op::Unpack:      return "unpack";
        case Op::ConstComplex: return "const.complex";
        case Op::IntConst:    return "int.const";
        case Op::ConstNull:   return "const.null";
        case Op::DelGlobal:   return "del.global";
        case Op::AddTraceback: return "add.traceback";
        case Op::MakeGenFunc: return "make.genfunc";
        case Op::MakeGenexp:  return "make.genexp";
        case Op::CellNew:     return "cell.new";
        case Op::CellGet:     return "cell.get";
        case Op::CellSet:     return "cell.set";
        case Op::MakeFunction: return "makefunc";
        case Op::LoadClassName: return "load.classname";
    }
    return "?";
}

static const char* own_name(Ownership o) {
    switch (o) {
        case Ownership::Owned:       return "owned";
        case Ownership::Borrowed:    return "borrowed";
        case Ownership::AlwaysNull:  return "null";
        case Ownership::NotAnObject: return "-";
        case Ownership::Unknown:     return "unknown";
    }
    return "?";
}

std::string to_string(const Module& m) {
    std::ostringstream o;
    o << "; module " << m.source_file << "\n";
    for (const Function& f : m.functions) {
        o << "\nfunc " << f.name << "(";
        for (std::size_t i = 0; i < f.params.size(); ++i)
            o << (i ? ", " : "") << f.params[i];
        o << ")";
        if (!f.locals.empty()) {
            o << "  ; locals:";
            for (const std::string& l : f.locals) o << " " << l;
        }
        if (!f.cellvars.empty()) {
            o << "  ; cells:";
            for (const std::string& l : f.cellvars) o << " " << l;
        }
        if (!f.freevars.empty()) {
            o << "  ; free:";
            for (const std::string& l : f.freevars) o << " " << l;
        }
        o << "\n";
        for (std::size_t bi = 0; bi < f.blocks.size(); ++bi) {
            const Block& b = f.blocks[bi];
            o << "  " << (b.label.empty() ? "bb" + std::to_string(bi) : b.label) << ":\n";
            for (const Instr& in : b.instrs) {
                o << "    ";
                if (in.result) o << "%" << in.result->id << " = ";
                o << op_name(in.op);
                if (in.op == Op::MakeGenexp || in.op == Op::MakeGenFunc) {
                    // The marshalled code object is arbitrary BYTES. The IR
                    // dump is a text artifact that tools read and diff, so it
                    // carries the blob's size, never its contents -- printing
                    // it raw made the listing invalid UTF-8.
                    o << " <code " << in.text.size() << " bytes>";
                } else if (!in.text.empty()) {
                    o << " \"" << in.text << "\"";
                }
                if (in.op == Op::Phi) {
                    for (std::size_t i = 0; i < in.args.size(); ++i)
                        o << (i ? ", " : " ") << "[%" << in.args[i].id << ", bb"
                          << in.phi_blocks[i] << "]";
                } else {
                    for (const Value& a : in.args) o << " %" << a.id;
                }
                for (std::uint32_t sid : in.stolen) o << " steals:%" << sid;
                if (in.has_imm) o << " #" << in.imm;
                if (in.op == Op::Br) o << " -> bb" << in.target;
                if (in.op == Op::CondBr)
                    o << " -> bb" << in.target << ", bb" << in.target_else;
                if (in.op == Op::IterNext)
                    o << " -> body bb" << in.target << ", done bb" << in.target_else;
                if (in.result && in.result_ownership != Ownership::NotAnObject)
                    o << "  ; " << own_name(in.result_ownership);
                if (in.on_error) {
                    // 0xFFFFFFFF marks "needs an error edge, not yet built".
                    // Printing it as a block number would imply a target that
                    // does not exist.
                    if (*in.on_error == 0xFFFFFFFFu) o << "  ; may raise (error edge TODO)";
                    else o << "  ; on_error -> bb" << *in.on_error;
                }
                o << "\n";
            }
        }
    }
    return o.str();
}

}  // namespace pyc::ir
