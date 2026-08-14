#!/usr/bin/env python3
"""W5.8 / I-014: boxed call sites must speculate into a specialized variant.

Parent (W4.3 / I-014 open): `generateSpecializedVariants` emits
`__specialized_add_ii`, but Codegen only calls it when every SSA arg is
already LLVM i64/double. A module-level `s = add(s, i)` loop loads
`pyc_global_s` as PyObject*, so the hot call stays boxed `@add`.

This check compiles that loop at -O0 --emit-llvm and requires:

1. `__specialized_add_ii` is defined (Compiler already does this).
2. The module entry (`pyc_user_main`) actually *calls* it.
3. That caller contains a runtime int-tag check (PyObject.type == 0),
   not a blind unbox. Accept either a GEP to field 1 + `icmp eq … 0`
   or a helper whose name mentions type/tag/isint.

Fails on the parent. After W5.8 it must pass. FILE_CASE / nbody
mismatches are a revert, not a weakening of this check.
"""
import os
import re
import subprocess
import sys
import tempfile


SRC = """\
def add(a, b):
    return a + b

s = 0
for i in range(10):
    s = add(s, i)
print(s)
"""


def find_pyc():
    candidates = [
        os.environ.get("PYC_BINARY"),
        os.path.join(os.getcwd(), "pyc"),
        os.path.join(os.getcwd(), "build", "pyc"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "pyc"),
    ]
    for c in candidates:
        if c and os.path.exists(c) and os.access(c, os.X_OK):
            return c
    print("ERROR: Could not find pyc binary. Set PYC_BINARY or build first.")
    sys.exit(1)


def functions(ll):
    parts = re.split(r"\ndefine ", ll)
    out = {}
    for part in parts[1:]:
        m = re.match(r"[^\n]*@([A-Za-z0-9_.]+)", part)
        if m:
            out[m.group(1)] = part
    return out


def has_int_tag_check(body):
    # PyObject.type is field 1 (i32). A speculate-int guard is icmp eq 0.
    gep_type = bool(re.search(
        r"getelementptr(?:\s+inbounds)?(?:\s+nuw)?\s+%PyObject,\s+ptr\s+\S+,\s+i32 0,\s+i32 1",
        body,
    ))
    icmp0 = bool(re.search(r"icmp eq i32\s+\S+,\s+0\b", body))
    helper = bool(re.search(
        r"call\s+\S+\s+@(?:Pyc_|PyObject_|pyc_)[A-Za-z0-9_]*"
        r"(?:IsInt|is_int|TypeTag|type_tag|GetType|GetTag)",
        body,
    ))
    return (gep_type and icmp0) or helper


def main():
    pyc = find_pyc()
    failed = 0
    with tempfile.TemporaryDirectory() as td:
        py = os.path.join(td, "repro.py")
        out = os.path.join(td, "repro")
        with open(py, "w") as f:
            f.write(SRC)
        c = subprocess.run(
            [pyc, py, "--emit-llvm", "-O0", "-o", out],
            capture_output=True, text=True, timeout=30,
        )
        ll_path = out + ".ll"
        if c.returncode != 0 or not os.path.exists(ll_path):
            print("FAIL compile --emit-llvm -O0")
            sys.stdout.write(c.stderr or c.stdout or "")
            sys.exit(1)
        ll = open(ll_path, encoding="utf-8").read()

    fns = functions(ll)
    if "__specialized_add_ii" not in fns:
        print("FAIL no define @__specialized_add_ii (Compiler should already emit it)")
        failed += 1
    else:
        print("PASS variant __specialized_add_ii exists")

    caller = None
    for name, body in fns.items():
        if name == "__specialized_add_ii":
            continue
        if re.search(r"call\s+\S+\s+@__specialized_add_ii\b", body):
            caller = name
            break
    if caller is None:
        print("FAIL no call to @__specialized_add_ii outside the variant "
              "(boxed site still takes @add)")
        failed += 1
    else:
        print("PASS %s calls __specialized_add_ii" % caller)
        if not has_int_tag_check(fns[caller]):
            print("FAIL %s calls the variant without a type==0 (or IsInt) guard"
                  % caller)
            failed += 1
        else:
            print("PASS %s has an int-tag check" % caller)

    if failed:
        print("check_speculative_unbox: %d check(s) failed" % failed)
        sys.exit(1)
    print("check_speculative_unbox: ok")
    sys.exit(0)


if __name__ == "__main__":
    main()
