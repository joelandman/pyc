#include "pyc/PythonParser.h"
#include <Python.h>
#include <fstream>
#include <sstream>
#include <iostream>

namespace pyc {

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

std::string getPyString(PyObject* obj, const char* attr) {
    PyObject* a = PyObject_GetAttrString(obj, attr);
    if (!a) return "";
    std::string s = PyUnicode_AsUTF8(a) ? PyUnicode_AsUTF8(a) : "";
    Py_DECREF(a);
    return s;
}

int getPyInt(PyObject* obj, const char* attr) {
    PyObject* a = PyObject_GetAttrString(obj, attr);
    int v = a ? PyLong_AsLong(a) : 0;
    Py_XDECREF(a);
    return v;
}

void buildAST(PyObject* pyNode, ASTNode* node) {
    if (!pyNode) return;
    PyObject* typeObj = (PyObject*)Py_TYPE(pyNode);
    node->type = getPyString(typeObj, "__name__");
    node->lineno = getPyInt(pyNode, "lineno");
    if (node->type == "Constant") {
        PyObject* v = PyObject_GetAttrString(pyNode, "value");
        if (v) {
            if (PyLong_Check(v)) {
                node->value = std::to_string(PyLong_AsLong(v));
                node->is_float = false;
            } else if (PyFloat_Check(v)) {
                double d = PyFloat_AsDouble(v);
                char buf[64];
                snprintf(buf, sizeof(buf), "%.17g", d);
                node->value = buf;
                node->is_float = true;
            } else {
                node->value = getPyString(pyNode, "value");
                node->is_float = false;
            }
            Py_DECREF(v);
        }
    } else if (node->type == "Name") {
        node->id = getPyString(pyNode, "id");
    } else if (node->type == "Attribute") {
        PyObject* value = PyObject_GetAttrString(pyNode, "value");
        if (value) {
            auto child = std::make_unique<ASTNode>();
            buildAST(value, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(value);
        }
        node->id = getPyString(pyNode, "attr");  // attribute name
    } else if (node->type == "BinOp") {
        PyObject* op = PyObject_GetAttrString(pyNode, "op");
        if (op) {
            PyObject* opType = (PyObject*)Py_TYPE(op);
            node->op = getPyString(opType, "__name__");
            Py_DECREF(op);
        }
        PyObject* left = PyObject_GetAttrString(pyNode, "left");
        if (left) {
            auto child = std::make_unique<ASTNode>();
            buildAST(left, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(left);
        }
        PyObject* right = PyObject_GetAttrString(pyNode, "right");
        if (right) {
            auto child = std::make_unique<ASTNode>();
            buildAST(right, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(right);
        }
    } else if (node->type == "FunctionDef") {
        node->id = getPyString(pyNode, "name");
        PyObject* a = PyObject_GetAttrString(pyNode, "args");
        if (a) {
            PyObject* argList = PyObject_GetAttrString(a, "args");
            if (argList && PyList_Check(argList)) {
                for (Py_ssize_t i = 0; i < PyList_Size(argList); ++i) {
                    PyObject* arg = PyList_GetItem(argList, i);
                    node->args.push_back(getPyString(arg, "arg"));
                }
            }
            // Handle default arguments — wrap the expression as a child of Default
            PyObject* defaults = PyObject_GetAttrString(a, "defaults");
            if (defaults && PyList_Check(defaults)) {
                for (Py_ssize_t i = 0; i < PyList_Size(defaults); ++i) {
                    PyObject* d = PyList_GetItem(defaults, i);
                    auto defNode = std::make_unique<ASTNode>();
                    defNode->type = "Default";
                    auto valueChild = std::make_unique<ASTNode>();
                    buildAST(d, valueChild.get());
                    defNode->children.push_back(std::move(valueChild));
                    node->children.push_back(std::move(defNode));
                }
            }
            Py_XDECREF(defaults);

            // Handle *args and **kwargs — check for None (absent) before pushing
            PyObject* vararg = PyObject_GetAttrString(a, "vararg");
            if (vararg && vararg != Py_None) {
                node->args.push_back("*" + getPyString(vararg, "arg"));
            }
            Py_XDECREF(vararg);
            PyObject* kwarg = PyObject_GetAttrString(a, "kwarg");
            if (kwarg && kwarg != Py_None) {
                node->args.push_back("**" + getPyString(kwarg, "arg"));
            }
            Py_XDECREF(kwarg);

            Py_XDECREF(argList); Py_DECREF(a);
        }
        // Always process body statements — cannot rely on the generic children.empty()
        // check because Default nodes are added to children above.
        PyObject* body = PyObject_GetAttrString(pyNode, "body");
        if (body && PyList_Check(body)) {
            for (Py_ssize_t i = 0; i < PyList_Size(body); ++i) {
                PyObject* item = PyList_GetItem(body, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(item, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(body);
    } else if (node->type == "Call") {
        PyObject* func = PyObject_GetAttrString(pyNode, "func");
        if (func) {
            auto child = std::make_unique<ASTNode>();
            buildAST(func, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(func);
        }
        PyObject* argsList = PyObject_GetAttrString(pyNode, "args");
        if (argsList && PyList_Check(argsList)) {
            for (Py_ssize_t i = 0; i < PyList_Size(argsList); ++i) {
                PyObject* arg = PyList_GetItem(argsList, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(arg, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(argsList);

        // Handle keyword arguments
        PyObject* keywords = PyObject_GetAttrString(pyNode, "keywords");
        if (keywords && PyList_Check(keywords)) {
            for (Py_ssize_t i = 0; i < PyList_Size(keywords); ++i) {
                PyObject* kw = PyList_GetItem(keywords, i);
                PyObject* arg = PyObject_GetAttrString(kw, "arg");
                PyObject* value = PyObject_GetAttrString(kw, "value");
                if (arg && value) {
                    auto kwNode = std::make_unique<ASTNode>();
                    kwNode->type = "Keyword";
                    // arg is a PyUnicode string (the keyword name), not an object with a .arg attr
                    kwNode->id = (arg && arg != Py_None && PyUnicode_Check(arg))
                                 ? (PyUnicode_AsUTF8(arg) ? PyUnicode_AsUTF8(arg) : "")
                                 : "";
                    auto valChild = std::make_unique<ASTNode>();
                    buildAST(value, valChild.get());
                    kwNode->children.push_back(std::move(valChild));
                    node->children.push_back(std::move(kwNode));
                }
                Py_XDECREF(arg);
                Py_XDECREF(value);
            }
        }
        Py_XDECREF(keywords);
    } else if (node->type == "If" || node->type == "While") {
        PyObject* test = PyObject_GetAttrString(pyNode, "test");
        if (test) {
            auto child = std::make_unique<ASTNode>();
            buildAST(test, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(test);
        }
        PyObject* body = PyObject_GetAttrString(pyNode, "body");
        if (body && PyList_Check(body)) {
            for (Py_ssize_t i = 0; i < PyList_Size(body); ++i) {
                PyObject* item = PyList_GetItem(body, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(item, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(body);
        if (node->type == "If") {
            PyObject* orelse = PyObject_GetAttrString(pyNode, "orelse");
            if (orelse && PyList_Check(orelse)) {
                for (Py_ssize_t i = 0; i < PyList_Size(orelse); ++i) {
                    PyObject* item = PyList_GetItem(orelse, i);
                    auto child = std::make_unique<ASTNode>();
                    buildAST(item, child.get());
                    node->children.push_back(std::move(child));
                }
            }
            Py_XDECREF(orelse);
        }
    } else if (node->type == "Compare") {
        PyObject* left = PyObject_GetAttrString(pyNode, "left");
        if (left) {
            auto child = std::make_unique<ASTNode>();
            buildAST(left, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(left);
        }
        PyObject* ops = PyObject_GetAttrString(pyNode, "ops");
        if (ops && PyList_Check(ops)) {
            PyObject* op = PyList_GetItem(ops, 0);
            if (op) {
                PyObject* opType = (PyObject*)Py_TYPE(op);
                node->op = getPyString(opType, "__name__");
            }
        }
        Py_XDECREF(ops);
        PyObject* comparators = PyObject_GetAttrString(pyNode, "comparators");
        if (comparators && PyList_Check(comparators) && PyList_Size(comparators) > 0) {
            PyObject* right = PyList_GetItem(comparators, 0);
            auto child = std::make_unique<ASTNode>();
            buildAST(right, child.get());
            node->children.push_back(std::move(child));
        }
        Py_XDECREF(comparators);
    } else if (node->type == "Assign") {
        PyObject* targets = PyObject_GetAttrString(pyNode, "targets");
        if (targets && PyList_Check(targets)) {
            PyObject* target = PyList_GetItem(targets, 0);
            if (target) {
                if (PyObject_HasAttrString(target, "id")) {
                    node->id = getPyString(target, "id");
                } else if (PyObject_HasAttrString(target, "value")) {
                    // Handle subscript assignment like x[0] = 5
                    node->id = "subscript";
                }
            }
        }
        Py_XDECREF(targets);
    } else if (node->type == "Subscript") {
        PyObject* value = PyObject_GetAttrString(pyNode, "value");
        if (value) {
            auto child = std::make_unique<ASTNode>();
            buildAST(value, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(value);
        }
        PyObject* slice = PyObject_GetAttrString(pyNode, "slice");
        if (slice) {
            auto child = std::make_unique<ASTNode>();
            buildAST(slice, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(slice);
        }
    } else if (node->type == "For") {
        PyObject* target = PyObject_GetAttrString(pyNode, "target");
        if (target) {
            // Handle target (e.g., variable name or subscript)
            if (PyObject_HasAttrString(target, "id")) {
                node->id = getPyString(target, "id");
            }
            Py_DECREF(target);
        }
        PyObject* iter = PyObject_GetAttrString(pyNode, "iter");
        if (iter) {
            auto child = std::make_unique<ASTNode>();
            buildAST(iter, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(iter);
        }
        PyObject* body = PyObject_GetAttrString(pyNode, "body");
        if (body && PyList_Check(body)) {
            for (Py_ssize_t i = 0; i < PyList_Size(body); ++i) {
                PyObject* item = PyList_GetItem(body, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(item, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(body);
    } else if (node->type == "Try") {
        PyObject* body = PyObject_GetAttrString(pyNode, "body");
        if (body && PyList_Check(body)) {
            for (Py_ssize_t i = 0; i < PyList_Size(body); ++i) {
                PyObject* item = PyList_GetItem(body, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(item, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(body);
        PyObject* handlers = PyObject_GetAttrString(pyNode, "handlers");
        if (handlers && PyList_Check(handlers)) {
            for (Py_ssize_t i = 0; i < PyList_Size(handlers); ++i) {
                PyObject* handler = PyList_GetItem(handlers, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(handler, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(handlers);
    } else if (node->type == "ExceptHandler") {
        PyObject* body = PyObject_GetAttrString(pyNode, "body");
        if (body && PyList_Check(body)) {
            for (Py_ssize_t i = 0; i < PyList_Size(body); ++i) {
                PyObject* item = PyList_GetItem(body, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(item, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(body);
    } else if (node->type == "List") {
        PyObject* elts = PyObject_GetAttrString(pyNode, "elts");
        if (elts && PyList_Check(elts)) {
            for (Py_ssize_t i = 0; i < PyList_Size(elts); ++i) {
                PyObject* elt = PyList_GetItem(elts, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(elt, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(elts);
    } else if (node->type == "Tuple") {
        PyObject* elts = PyObject_GetAttrString(pyNode, "elts");
        if (elts && PyList_Check(elts)) {
            for (Py_ssize_t i = 0; i < PyList_Size(elts); ++i) {
                PyObject* elt = PyList_GetItem(elts, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(elt, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(elts);
    } else if (node->type == "Dict") {
        PyObject* keys = PyObject_GetAttrString(pyNode, "keys");
        PyObject* values = PyObject_GetAttrString(pyNode, "values");
        if (keys && values && PyList_Check(keys) && PyList_Check(values)) {
            size_t numPairs = PyList_Size(keys) < PyList_Size(values) ? 
                              PyList_Size(keys) : PyList_Size(values);
            for (Py_ssize_t i = 0; i < numPairs; ++i) {
                PyObject* key = PyList_GetItem(keys, i);
                PyObject* val = PyList_GetItem(values, i);
                auto keyChild = std::make_unique<ASTNode>();
                buildAST(key, keyChild.get());
                auto valChild = std::make_unique<ASTNode>();
                buildAST(val, valChild.get());
                node->children.push_back(std::move(keyChild));
                node->children.push_back(std::move(valChild));
            }
        }
        Py_XDECREF(keys);
        Py_XDECREF(values);
    } else if (node->type == "Set") {
        PyObject* elts = PyObject_GetAttrString(pyNode, "elts");
        if (elts && PyList_Check(elts)) {
            for (Py_ssize_t i = 0; i < PyList_Size(elts); ++i) {
                PyObject* elt = PyList_GetItem(elts, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(elt, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(elts);
    } else if (node->type == "Starred") {
        // Handle *args in function calls
        PyObject* value = PyObject_GetAttrString(pyNode, "value");
        if (value) {
            auto child = std::make_unique<ASTNode>();
            buildAST(value, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(value);
        }
    } else if (node->type == "ListComp") {
        // Handle list comprehension: [elt for target in iter if ifs]
        PyObject* elt = PyObject_GetAttrString(pyNode, "elt");
        if (elt) {
            auto child = std::make_unique<ASTNode>();
            buildAST(elt, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(elt);
        }
        PyObject* generators = PyObject_GetAttrString(pyNode, "generators");
        if (generators && PyList_Check(generators)) {
            for (Py_ssize_t i = 0; i < PyList_Size(generators); ++i) {
                PyObject* gen = PyList_GetItem(generators, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(gen, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(generators);
    } else if (node->type == "DictComp") {
        // Handle dict comprehension: {key: value for target in iter if ifs}
        PyObject* key = PyObject_GetAttrString(pyNode, "key");
        if (key) {
            auto child = std::make_unique<ASTNode>();
            buildAST(key, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(key);
        }
        PyObject* value = PyObject_GetAttrString(pyNode, "value");
        if (value) {
            auto child = std::make_unique<ASTNode>();
            buildAST(value, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(value);
        }
        PyObject* generators = PyObject_GetAttrString(pyNode, "generators");
        if (generators && PyList_Check(generators)) {
            for (Py_ssize_t i = 0; i < PyList_Size(generators); ++i) {
                PyObject* gen = PyList_GetItem(generators, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(gen, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(generators);
    } else if (node->type == "comprehension") {
        // Handle comprehension node: target, iter, ifs
        PyObject* target = PyObject_GetAttrString(pyNode, "target");
        if (target) {
            auto child = std::make_unique<ASTNode>();
            buildAST(target, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(target);
        }
        PyObject* iter = PyObject_GetAttrString(pyNode, "iter");
        if (iter) {
            auto child = std::make_unique<ASTNode>();
            buildAST(iter, child.get());
            node->children.push_back(std::move(child));
            Py_DECREF(iter);
        }
        PyObject* ifs = PyObject_GetAttrString(pyNode, "ifs");
        if (ifs && PyList_Check(ifs)) {
            for (Py_ssize_t i = 0; i < PyList_Size(ifs); ++i) {
                PyObject* ifCond = PyList_GetItem(ifs, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(ifCond, child.get());
                node->children.push_back(std::move(child));
            }
        }
        Py_XDECREF(ifs);
    }
    if (node->children.empty()) {
        PyObject* body = PyObject_GetAttrString(pyNode, "body");
        if (body && PyList_Check(body)) {
            for (Py_ssize_t i = 0; i < PyList_Size(body); ++i) {
                PyObject* item = PyList_GetItem(body, i);
                auto child = std::make_unique<ASTNode>();
                buildAST(item, child.get());
                node->children.push_back(std::move(child));
            }
        } else {
            PyObject* val = PyObject_GetAttrString(pyNode, "value");
            if (val) {
                auto child = std::make_unique<ASTNode>();
                buildAST(val, child.get());
                node->children.push_back(std::move(child));
                Py_DECREF(val);
            }
        }
        Py_XDECREF(body);
    }
}

std::unique_ptr<ASTNode> PythonParser::parseFile(const std::string& path) {
    return parse(readFile(path), path);
}

std::unique_ptr<ASTNode> PythonParser::parse(const std::string& source, const std::string& filename) {
    if (!Py_IsInitialized()) Py_Initialize();
    PyObject* astModule = PyImport_ImportModule("ast");
    if (!astModule) { PyErr_Print(); return nullptr; }
    PyObject* parseFunc = PyObject_GetAttrString(astModule, "parse");
    PyObject* args = PyTuple_Pack(1, PyUnicode_FromString(source.c_str()));
    PyObject* kwargs = PyDict_New();
    PyDict_SetItemString(kwargs, "filename", PyUnicode_FromString(filename.c_str()));
    PyObject* pyAst = PyObject_Call(parseFunc, args, kwargs);
    Py_DECREF(args); Py_DECREF(kwargs); Py_DECREF(parseFunc); Py_DECREF(astModule);
    if (!pyAst) { PyErr_Print(); return nullptr; }
    auto root = std::make_unique<ASTNode>();
    buildAST(pyAst, root.get());
    Py_DECREF(pyAst);
    return root;
}

} // namespace pyc
