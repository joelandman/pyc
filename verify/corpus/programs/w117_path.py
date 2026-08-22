# W10.2 / I-117: pathlib PurePath, .parts, .resolve, .glob, multi-arg Path.
from pathlib import Path, PurePath
import os
p = Path("a", "b", "c")
print(p.name)
print(p.parts)
print(str(p))
pp = PurePath("x", "y")
print(pp.name)
print(str(pp))
print(Path("/tmp", "foo").parts)
base = "/tmp/pyc_w117_glob"
os.makedirs(base, exist_ok=True)
with open(base + "/a.txt", "w") as f:
    f.write("x")
with open(base + "/b.py", "w") as f:
    f.write("y")
with open(base + "/c.txt", "w") as f:
    f.write("z")
names = [x.name for x in Path(base).glob("*.txt")]
print(sorted(names))
r = Path(base + "/a.txt").resolve()
print(r.name)
print(str(r).endswith("a.txt"))
print(Path(base).joinpath("a.txt").exists())
