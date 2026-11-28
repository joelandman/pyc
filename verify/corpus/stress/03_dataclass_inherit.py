# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: dataclass inheritance, field override, MRO, init=False.
from dataclasses import dataclass, field


@dataclass
class Base:
    a: int = 1
    b: str = "b"


@dataclass
class Sub(Base):
    b: str = "overridden"
    c: float = 2.0


s = Sub()
print(s.a, s.b, s.c)
print(isinstance(s, Base))
print(issubclass(Sub, Base))


@dataclass
class WithSecret:
    x: int
    token: str = field(default="t", init=False)


w = WithSecret(5)
print(w.x, w.token)
