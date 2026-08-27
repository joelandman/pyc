# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# PEP 649. The two halves are independent: the ASSIGNMENT happens whenever a
# value is present; the ANNOTATION is never evaluated where it is written, and
# is recorded only for a simple name in a class or module body. The scope gets
# an __annotate__(format) function and CPython's own descriptor calls it when
# __annotations__ is read.
import sys

x: int = 5
y: str                              # records the annotation, binds nothing

m = sys.modules[__name__]
print("x =", x)
print("y bound?", "y" in vars(m))
print("module annotations:", m.__annotations__)


def fn():
    a: int = 3
    b: float                        # nothing at all happens here
    return a


print("function =", fn())


class Plain:
    p: int = 1
    q: bool


print("class:", Plain.p, Plain.__annotations__)


class Deferred:
    n: NotDefinedAnywhere = 0       # must NOT raise: the annotation is lazy


print("deferred value:", Deferred.n)
try:
    Deferred.__annotations__
except NameError as e:
    print("evaluating it raises:", e)


class Scoped:
    local = int
    z: local                        # reads a name bound in the class body


print("class-body scope:", Scoped.__annotations__)


class Box:
    pass


o = Box()
o.attr: AlsoUndefined = 7           # assigns; annotation not evaluated
d = {}
d["k"]: AlsoUndefined = 9
print("attribute =", o.attr, "subscript =", d["k"])


class Order:
    a: int = 1
    b: str
    c: float = 2.0


print("order preserved:", list(Order.__annotations__))


class Compound:
    if True:
        e: int = 1
    try:
        f: str = "x"
    except Exception:
        pass


print("compound statements:", sorted(Compound.__annotations__))
