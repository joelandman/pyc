# corpus case — ground truth is CPython at run time (CHARTER I5).
import os
import pathlib
from pathlib import Path

scratch = "/tmp/pyc_test_os_pathlib_scratch"

print(os.path.join("a", "b", "c"))
print(os.path.basename("/x/y/z.txt"))
print(os.path.dirname("/x/y/z.txt"))
print(list(os.path.splitext("/x/y/z.tar.gz")))

p = pathlib.Path(scratch)
sub = p / "nested" / "dir"
sub.mkdir(parents=True, exist_ok=True)
print(sub.is_dir())

f = sub / "hello.txt"
with open(str(f), "w") as fh:
    fh.write("hi")
print(f.exists(), f.is_file(), f.is_dir())
print(f.name, f.suffix, f.stem)
print(f.parent == sub)

os.remove(str(f))
print(f.exists())

names = sorted(os.listdir(str(sub.parent)))
print(names)

q = Path("x").joinpath("y", "z")
print(q)

def name_of(x):
    return x.name
def show(x):
    return str(x)
print(name_of(p), show(p))
# pathlib method calls through untyped params (fixed: was returning None)
def exists_of(x):
    return x.exists()
def is_dir_of(x):
    return x.is_dir()
def join_of(x, y):
    return x.joinpath(y)
print(exists_of(f), is_dir_of(sub))
print(join_of(p, "child"))

print(isinstance(os.getcwd(), str))
print(len(os.environ) >= 0)
