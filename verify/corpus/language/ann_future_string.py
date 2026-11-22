# from __future__ import annotations — STRING annotation format regression.
#
# Gated: this program MUST stay byte-identical to the oracle. It exercises the
# string path that the existing ann_* probes never touch (they cover the eager
# path only): under the flag, every annotation is stored as its canonical source
# string, and an UNRESOLVABLE annotation must NOT crash. A revert to the eager
# path would evaluate `Missing` -> NameError, or store the bare evaluated value
# instead of the quoted string -- both diverge from the oracle here.

from __future__ import annotations

x: "Missing" = 1                 # module-level, unresolvable -> stays a string
y: int = 2

class C:
    z: "AlsoMissing" = 1         # class-level, unresolvable

def f(a: "Missing", b: "list[int]" = 1) -> "dict[str, int]":
    pass

import sys
m = sys.modules["__main__"]
print("module:", dict(sorted(m.__annotations__.items())))
print("class :", dict(sorted(C.__annotations__.items())))
print("func  :", dict(sorted(f.__annotations__.items())))
