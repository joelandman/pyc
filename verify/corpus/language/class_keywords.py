# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# `class C(bases, metaclass=M, **kw)`. metaclass= is consumed by class
# creation and must NOT reach the metaclass call or __init_subclass__; every
# other keyword reaches both. The namespace comes from M.__prepare__, which is
# required rather than optional: enum.EnumMeta.__prepare__ returns an
# _EnumDict that records member order, and a plain dict yields an enum that is
# wrong rather than one that fails.
import abc
import enum


class Meta(type):
    def __new__(mcls, name, bases, ns, **kw):
        cls = super().__new__(mcls, name, bases, ns)
        cls.extra = kw
        return cls


class A(metaclass=Meta):
    pass


class B(metaclass=Meta, x=1, y=2):
    pass


print("explicit:", type(A).__name__, A.extra)
print("keywords:", B.extra)


class Abstract(abc.ABC):
    @abc.abstractmethod
    def go(self): ...


print("inherited metaclass:", type(Abstract).__name__)
try:
    Abstract()
except TypeError as e:
    print("abstract refuses:", str(e)[:40])


class Colour(enum.Enum):
    RED = 1
    GREEN = 2


print("enum:", list(Colour), Colour.RED.value)


class Flags(enum.Flag, boundary=enum.STRICT):
    A = 1
    B = 2


print("enum keyword:", Flags.A | Flags.B)

kw = {"metaclass": Meta, "z": 3}


class Splat(**kw):
    pass


print("**kwargs:", type(Splat).__name__, Splat.extra)
