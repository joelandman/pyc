#!/usr/bin/env python3
"""Static guard against shadowed arms in Compiler.cpp's lowerMethodCall.

The method-dispatch chain matches on method *name*, with each arm
optionally gated on typeOf(obj). If an arm's guard is just the name -- a
catch-all -- any later arm for that same name in the same chain is
unreachable.

Not hypothetical: Counter.update() was given its own runtime function in
the chain's terminal else, an earlier untyped `update` arm resolved
first, and the new code was dead on arrival while every Counter.update()
silently took the plain replacing dict merge. A dead arm looks exactly
like a working one until the semantics differ, so the failure is
invisible in review and invisible at runtime.

THE RULE, and why it is this one:

  Group arms by brace depth (which separates the main chain from the
  nested `re.`/`math.` module dispatchers). Within a group, in file
  order, every occurrence of a method name except the last must carry a
  narrowing guard.

This is deliberately a *syntactic* rule, not a control-flow one. Earlier
attempts modelled if/else-if chains and dominance properly; every
version was either over-permissive (masking the very bug this exists to
catch) or under-permissive (flagging sequential independent `if`s and
arms guarded by an enclosing block). Reliably recovering reachability
from C++ text turned out to need far more than brace counting, and a
checker that is confidently wrong in either direction is worse than no
checker. So: a blunt rule that is easy to reason about, plus a short
explicit exemption list that is printed on every run so it cannot rot
silently.

The sound long-term fix is to stop inferring this from source text --
build the arms as an ordered (name, predicate) table in C++ so a
duplicate-name-after-catch-all check is a loop over data, not a parse.
See IMPLEMENTATION.md, "Method dispatch".

Run directly, or via tests/runner.py which invokes it as a test case.
"""
import os
import re
import sys

COMPILER = os.path.join(os.path.dirname(__file__), "..", "src", "Compiler.cpp")

ARM_HEAD = re.compile(r'^\s*\}?\s*(?:else\s+)?if\s*\(')
METHOD_ARM = re.compile(r'methodName\s*==\s*"([A-Za-z_0-9]+)"')

# A guard counts as narrowing if it constrains the arm beyond the bare
# method name.
NARROWING = ("typeOf(", "isProven", "isCollectionsModule", "args.size()",
             "args.empty()", "hasKeywordArgs", "->type ==", "->id ==",
             "Aliases", "isShadowedLocal", "attr->children", "sepIsNone",
             "isRealDictReceiver", "isOsPathReceiver",
             "isImportedModuleReceiver", "isKnownClassReceiver")

# Known-benign catch-alls: the arm itself has no receiver-type guard, but
# an ENCLOSING block supplies one, which this rule deliberately does not
# model. Each entry must say which enclosing guard makes it safe. Every
# exemption is printed on each run; if one stops being needed, delete it.
EXEMPT = {
    # `exists` was removed in W2.3: pathlib exists is now
    # `methodName == "exists" && typeOf(obj) == "path"` (narrowed), and
    # os.path.exists is gated on AST module identity. The exemption no
    # longer fired after I-006.
    "compile": "both arms are inside the `re.`-module block and run in "
               "sequence (the first picks the runtime fn, the second adds "
               "flags handling) -- independent `if`s, not an else-chain",
}


def strip_strings(line):
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)


def condition_text(lines, i):
    """Exact condition starting at line i, accumulated until parens balance.

    Reading a fixed number of following lines instead pulls in the arm's
    body, and bodies routinely mention args.empty()/args.size() -- which
    made real catch-alls look narrowed and let the bug through.
    """
    parts, par, started = [], 0, False
    for j in range(i, min(i + 12, len(lines))):
        s = strip_strings(lines[j])
        parts.append(lines[j])
        for ch in s:
            if ch == "(":
                par += 1
                started = True
            elif ch == ")":
                par -= 1
        if started and par <= 0:
            break
    return " ".join(parts)


def scan(path):
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().split("\n")

    depth, groups = 0, {}
    for i, raw in enumerate(lines):
        if ARM_HEAD.match(raw):
            m = METHOD_ARM.search(raw)
            if m:
                cond = condition_text(lines, i)
                groups.setdefault(depth, []).append({
                    "name": m.group(1),
                    "line": i + 1,
                    "narrowed": any(t in cond for t in NARROWING),
                })
        s = strip_strings(raw)
        depth += s.count("{") - s.count("}")
    return groups


def main():
    groups = scan(COMPILER)
    total = sum(len(v) for v in groups.values())
    if not total:
        print("check_dispatch_chain: FAIL - parsed 0 arms; the pattern no "
              "longer matches lowerMethodCall's shape")
        return 1

    violations, exempted = [], []
    for depth, arms in sorted(groups.items()):
        by_name = {}
        for arm in arms:
            by_name.setdefault(arm["name"], []).append(arm)
        for name, occ in sorted(by_name.items()):
            for idx, arm in enumerate(occ[:-1]):
                if arm["narrowed"]:
                    continue
                record = (name, depth, arm, [o for o in occ[idx + 1:]])
                (exempted if name in EXEMPT else violations).append(record)

    for name, depth, arm, shadowed in exempted:
        print(f"check_dispatch_chain: exempt '{name}' @Compiler.cpp:"
              f"{arm['line']} - {EXEMPT[name]}")

    if violations:
        print("\ncheck_dispatch_chain: FAIL - unreachable arm(s) in "
              "lowerMethodCall\n")
        for name, depth, arm, shadowed in violations:
            print(f"  '{name}': catch-all arm at Compiler.cpp:{arm['line']} "
                  f"(no receiver-type guard) shadows:")
            for b in shadowed:
                print(f"      Compiler.cpp:{b['line']}  UNREACHABLE")
            print("    Fix: give the earlier arm a receiver-type whitelist so "
                  "unproven\n         receivers fall through, or fold the "
                  "later arm's behavior into it.\n")
        print(f"{total} arms checked, {len(violations)} violation(s)")
        return 1

    print(f"check_dispatch_chain: OK - {total} arms, no shadowed arms "
          f"({len(exempted)} documented exemption(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
