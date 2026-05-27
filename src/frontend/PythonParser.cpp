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
            } else {
                node->value = getPyString(pyNode, "value");
            }
            Py_DECREF(v);
        }
    } else if (node->type == "Name") {
        node->id = getPyString(pyNode, "id");
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
            Py_XDECREF(argList); Py_DECREF(a);
        }
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
            if (target) node->id = getPyString(target, "id");
        }
        Py_XDECREF(targets);
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
