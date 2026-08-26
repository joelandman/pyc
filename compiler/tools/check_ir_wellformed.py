#!/usr/bin/env python3
"""Structural checks on emitted IR.

Cheap invariants that a malformed lowering violates long before codegen does,
where the failure would surface as an opaque "Module verification failed" --
the same class the old tree hit on `def g(a, /, b)`.

  * every block ends with exactly one terminator
  * every branch target exists
  * every value is defined before use
  * every owned result is released on the normal path exactly once

    ./compiler/tools/check_ir_wellformed.py --lower /tmp/pyc_lower compiler/tests/*.py
"""
from __future__ import annotations

import argparse, re, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import selfcheck as sc

GRN, RED, DIM, BOLD, RST = "\033[32m", "\033[31m", "\033[90m", "\033[1m", "\033[0m"
# Terminators. This list must track ir::Op -- codegen has its own is_term()
# and the two are coupled by nothing but attention, which has now cost three
# false failures (ret, phi, iter.next). Worth deriving from one source if it
# happens again.
# Terminators. iter.next has a RESULT (the item), so the optional `%N = `
# prefix is not cosmetic -- without it the terminator is invisible and every
# for.head block reads as unterminated.
#
# This list must track ir::Op. Codegen has its own is_term() and the two are
# coupled by nothing but attention, which has now cost three false failures
# (ret, phi, iter.next). Worth deriving from one source if it happens again.
TERM = re.compile(r"^    (?:%\d+ = )?(br|condbr|ret|ret\.err|iter\.next|raise)\b")
BLOCK = re.compile(r"^  (\S+):$")
QUOTED = re.compile(r'"(?:[^"\\\\]|\\\\.)*"')
DEF = re.compile(r"^    %(\d+) = ")
USE = re.compile(r"%(\d+)")
DECREF = re.compile(r"^    decref %(\d+)$")
# `ret %N` hands the reference to the caller: ownership is TRANSFERRED, not
# leaked. Counting it as a leak is a false positive, and a checker that cries
# wolf gets ignored just as surely as one that stays silent.
RET_VAL = re.compile(r"^    ret %(\d+)$")
# A phi CONSUMES its incoming values: ownership transfers to the phi result,
# which is then released once for whichever path was taken. Counting the
# operands as leaked is the same false positive as `ret %N` was.
PHI = re.compile(r"^    %\d+ = phi ")
PHI_IN = re.compile(r"\[%(\d+),")
OWNED = re.compile(r"; owned")
# A STOLEN reference is released by the callee, so no decref will ever appear
# for it. The IR records which operands a call steals (from §4's table), so the
# checker reads it there rather than keeping a second copy of the steal list
# that would drift the moment the table changed.
STEAL_IN = re.compile(r"steals:%(\d+)")


def is_pad(block: str) -> bool:
    """Landing pads are named unwind.N by make_landing_pad. Coupled to the
    lowerer by that convention alone -- if the naming changes, this check
    silently stops finding anything, so the self-check below must keep proving
    it can still fail."""
    return block.startswith("unwind.")


def check(ir: str) -> list[str]:
    problems: list[str] = []
    block, terms, nblocks = None, 0, 0
    defined: set[str] = set()
    owned: set[str] = set()
    released: dict[str, int] = {}
    # Which block each owned value is defined in, and which blocks release it.
    # Without this, a value released ONLY inside unwind pads looks released.
    def_block: dict[str, str] = {}
    release_blocks: dict[str, set[str]] = {}
    # A block ending in `raise` or `ret.err` has NO normal exit -- codegen:
    # "Always fails, so control always leaves by the error edge." Values born
    # there and released on that edge are correct, not leaks.
    no_normal_exit: set[str] = set()
    deferred: list[str] = []           # phi operands, checked once all defs are in
    for line in ir.splitlines():
        m = BLOCK.match(line)
        if m:
            if block is not None and terms != 1:
                problems.append(f"block '{block}' has {terms} terminators")
            block, terms = m.group(1), 0
            nblocks += 1
            continue
        if TERM.match(line):
            terms += 1
            if re.match(r"\s*(raise|ret\.err)\b", line) and block:
                no_normal_exit.add(block)
        d = DEF.match(line)
        if d:
            defined.add(d.group(1))
            if OWNED.search(line):
                owned.add(d.group(1))
                def_block[d.group(1)] = block or "?"
        r = DECREF.match(line) or RET_VAL.match(line)
        if r:
            released[r.group(1)] = released.get(r.group(1), 0) + 1
            release_blocks.setdefault(r.group(1), set()).add(block or "?")
        for st in STEAL_IN.findall(line):
            released[st] = released.get(st, 0) + 1
            release_blocks.setdefault(st, set()).add(block or "?")
        if PHI.match(line):
            for inc in PHI_IN.findall(line):
                released[inc] = released.get(inc, 0) + 1
            # A phi operand comes from its PREDECESSOR, which may be emitted
            # later in the listing -- try/finally converges the normal, unwind
            # and return paths on one block, so its phis necessarily reference
            # values defined further down. Textual order is the wrong test for
            # these; defer them and require only that they are defined SOMEWHERE
            # in the function, which still catches a phi over a nonexistent
            # value.
            for inc in PHI_IN.findall(line):
                deferred.append(inc)
            continue
        # Strip quoted text before looking for %N value references: a format
        # string like "[%10s]" is not a use of %10, and sprintf.py's "%05"/"%02"
        # read the same way. This is only reliable because ir.cpp now escapes
        # string contents, so a quote inside a constant cannot end the region.
        scan = QUOTED.sub('""', line.split("=")[-1] if d else line)
        for u in USE.findall(scan):
            # %0 is the null sentinel (a nullable C-API argument such as
            # PySet_New(NULL)), not a value, so it has no definition.
            if u != "0" and u not in defined:
                problems.append(f"value %{u} used before definition")
    if block is not None and terms != 1:
        problems.append(f"block '{block}' has {terms} terminators")
    for u in deferred:
        if u != "0" and u not in defined:
            problems.append(f"phi operand %{u} is never defined")
    # Owned values must be released. Landing pads mean a value can legitimately
    # be released more than once ACROSS paths, so only never-released is an
    # unambiguous defect from this vantage point.
    for v in sorted(owned, key=int):
        if v not in released:
            problems.append(f"owned %{v} is never released or returned (leak)")
            continue
        # Released, but ONLY on unwind paths. A value produced on the normal
        # path and decref-ed only inside landing pads leaks on every run that
        # does not raise -- which is most of them. This is how the match-helper
        # leak (pyc_rt_match_sequence, an owned result released only in its
        # failure pads) passed as "ok": the check above sees it in `released`
        # and stops there.
        #
        # Only flag values DEFINED outside a pad: one created inside a pad and
        # released there is correct.
        if is_pad(def_block.get(v, "")):
            continue
        if def_block.get(v, "") in no_normal_exit:
            continue
        blocks = release_blocks.get(v, set())
        if blocks and all(is_pad(b) for b in blocks):
            where = ", ".join(sorted(blocks)[:3])
            problems.append(
                f"owned %{v} (defined in '{def_block.get(v, '?')}') is released "
                f"only on unwind paths [{where}] -- leaks on the normal path")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lower", required=True)
    ap.add_argument("--parser-cwd", default="compiler")
    ap.add_argument("--python", default="python3")
    ap.add_argument("files", nargs="+", type=Path)
    args = ap.parse_args()

    # Assume the instrument is broken until it shows it can produce both
    # answers. Each probe below is a defect this checker claims to catch.
    sc.require_detects("two terminators", check,
        "; m\nfunc f()\n  entry:\n    %1 = const.int \"1\"  ; owned\n"
        "    decref %1\n    br -> bb1\n    br -> bb2\n",
        "; m\nfunc f()\n  entry:\n    %1 = const.int \"1\"  ; owned\n"
        "    decref %1\n    ret\n")
    sc.require_detects("leaked owned value", check,
        "; m\nfunc f()\n  entry:\n    %1 = const.int \"1\"  ; owned\n    ret\n",
        "; m\nfunc f()\n  entry:\n    %1 = const.int \"1\"  ; owned\n    ret %1\n")
    # The blind spot this check was added to close: an owned value released
    # ONLY inside landing pads. The bad input mirrors the real shape -- a value
    # produced in `entry`, decref-ed in an `unwind.` pad, and never on the path
    # that returns. The good input releases it on the normal path too.
    sc.require_detects("owned value released only on unwind paths", check,
        "; m\nfunc f()\n  entry:\n    %1 = const.int \"1\"  ; owned\n    ret\n"
        "  unwind.0:\n    decref %1\n    ret.err\n",
        "; m\nfunc f()\n  entry:\n    %1 = const.int \"1\"  ; owned\n"
        "    decref %1\n    ret\n"
        "  unwind.0:\n    decref %1\n    ret.err\n")
    sc.require_detects("use before definition", check,
        "; m\nfunc f()\n  entry:\n    %2 = call.capi \"PyNumber_Add\" %9 %9  ; owned\n"
        "    decref %2\n    ret\n",
        "; m\nfunc f()\n  entry:\n    %1 = const.int \"1\"  ; owned\n    ret %1\n")

    ok = bad = unsup = 0
    outcomes = []
    for f in args.files:
        e = subprocess.run([args.python, "-m", "pyc_parse", str(f.resolve())],
                           cwd=args.parser_cwd, capture_output=True, text=True)
        if e.returncode:
            print(f"  {f.name:10s} {DIM}parse failed{RST}"); bad += 1
            outcomes.append("parse"); continue
        r = subprocess.run([args.lower, "-"], input=e.stdout,
                           capture_output=True, text=True)
        if r.returncode:
            unsup += 1
            outcomes.append("unsupported")
            print(f"  {f.name:10s} {DIM}unsupported: "
                  f"{r.stderr.strip().splitlines()[0][:56] if r.stderr.strip() else '?'}{RST}")
            continue
        probs = check(r.stdout)
        outcomes.append("malformed" if probs else "ok")
        if probs:
            bad += 1
            print(f"  {f.name:10s} {RED}MALFORMED{RST}")
            for pr in probs[:6]:
                print(f"      {pr}")
        else:
            ok += 1
            print(f"  {f.name:10s} {GRN}ok{RST}")
    sc.reject_implausible_uniformity(outcomes, what="files")
    print(f"\n  well-formed {ok}   malformed {bad}   unsupported {unsup}")

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
