"""GDB pretty-printer for pyc PyObject* (I-019 / W3.4).

Load after compiling with ``pyc foo.py -o foo -g -O0``:

    gdb ./foo
    (gdb) source /path/to/pyc/tools/pyc_gdb.py
    (gdb) break greet
    (gdb) run
    (gdb) print x        # 42  instead of  (PyObject *) 0x...

Or add to ``~/.gdbinit``:

    source /path/to/pyc/tools/pyc_gdb.py

Scalars (int / float / bool / None) read the C fields of PyObject.
str / list / dict / tuple use libstdc++ pretty-printers when GDB has
them; otherwise they print as ``<str>`` / ``<list>``. Tag 5 is both
bool and None (I-013): ``str == "None"`` wins. Tag 7 is both tuple
and super-proxy: a non-empty ``str`` is treated as super.
"""

import gdb


_TYPE_NAMES = {
    0: "int",
    1: "list",
    2: "dict",
    3: "str",
    4: "float",
    5: "bool",
    6: "cell",
    7: "tuple",
    8: "regex",
    9: "match",
    10: "exception",
    11: "function",
    12: "exception_class",
    13: "complex",
    14: "datetime",
    15: "timedelta",
    16: "path",
    17: "bytes",
    18: "bytearray",
    19: "decimal",
    20: "set",
}


def _as_int(val):
    try:
        return int(val)
    except (gdb.error, ValueError, TypeError):
        return None


def _cpp_string(val):
    """Best-effort std::string → Python str."""
    try:
        return val.string()
    except (gdb.error, UnicodeDecodeError, ValueError):
        pass
    for field in ("_M_p",):
        try:
            p = val["_M_dataplus"][field]
            return p.string()
        except (gdb.error, UnicodeDecodeError, ValueError, KeyError):
            continue
    try:
        s = str(val)
        if s.startswith('"') and s.endswith('"'):
            return s[1:-1]
        return None
    except Exception:
        return None


def _vec_size(val):
    try:
        return int(val["_M_impl"]["_M_finish"] - val["_M_impl"]["_M_start"])
    except (gdb.error, ValueError, TypeError, KeyError):
        return None


class PyObjectPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        v = self.val
        if v.type.code == gdb.TYPE_CODE_PTR:
            if int(v) == 0:
                return "None"
            try:
                v = v.dereference()
            except gdb.error:
                return "<PyObject* %s>" % self.val

        tag = _as_int(v["type"])
        if tag is None:
            return "<PyObject>"

        if tag == 0:
            n = _as_int(v["value"])
            return str(n) if n is not None else "<int>"

        if tag == 4:
            try:
                return repr(float(v["dvalue"]))
            except (gdb.error, ValueError, TypeError):
                return "<float>"

        if tag == 5:
            s = _cpp_string(v["str"])
            if s == "None":
                return "None"
            n = _as_int(v["value"])
            return "True" if n else "False"

        if tag == 3:
            s = _cpp_string(v["str"])
            return repr(s) if s is not None else "<str>"

        if tag == 13:
            try:
                re = float(v["complex_real"])
                im = float(v["complex_imag"])
                return "(%s%+sj)" % (re, im)
            except (gdb.error, ValueError, TypeError):
                return "<complex>"

        if tag == 7:
            s = _cpp_string(v["str"])
            if s:
                return "<super>"
            n = _vec_size(v["list"])
            return "<tuple len=%d>" % n if n is not None else "<tuple>"

        if tag == 1:
            item = _as_int(v["list_item_type"])
            if item == 1:
                n = _vec_size(v["ilist"])
                return "<list[int] len=%d>" % n if n is not None else "<list[int]>"
            if item == 2:
                n = _vec_size(v["flist"])
                return "<list[float] len=%d>" % n if n is not None else "<list[float]>"
            n = _vec_size(v["list"])
            return "<list len=%d>" % n if n is not None else "<list>"

        if tag == 2:
            n = _vec_size(v["dict"])
            return "<dict len=%d>" % n if n is not None else "<dict>"

        if tag == 20:
            n = _vec_size(v["list"])
            return "<set len=%d>" % n if n is not None else "<set>"

        if tag == 17 or tag == 18:
            s = _cpp_string(v["str"])
            kind = "bytes" if tag == 17 else "bytearray"
            if s is None:
                return "<%s>" % kind
            return "%s(%r)" % (kind, s.encode("latin1", "replace") if isinstance(s, str) else s)

        if tag == 10:
            s = _cpp_string(v["str"])
            return "<%s>" % (s or "exception")

        if tag == 11:
            s = _cpp_string(v["str"])
            return "<function %s>" % (s or "?")

        if tag == 12:
            s = _cpp_string(v["str"])
            return "<class %s>" % (s or "?")

        name = _TYPE_NAMES.get(tag, "type%d" % tag)
        return "<PyObject %s>" % name


def _lookup(val):
    t = val.type
    if t.code == gdb.TYPE_CODE_PTR:
        t = t.target()
    try:
        if t.name == "PyObject" or t.strip_typedefs().name == "PyObject":
            return PyObjectPrinter(val)
    except (gdb.error, RuntimeError, AttributeError):
        pass
    return None


def register():
    gdb.pretty_printers = [p for p in gdb.pretty_printers if getattr(p, "__name__", "") != "_lookup"]
    gdb.pretty_printers.append(_lookup)


register()
