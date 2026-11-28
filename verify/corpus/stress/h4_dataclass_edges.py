# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: dataclass KW_ONLY, field(repr=False), field(init=False) combos and
# the field-order constraints (kw_only must follow non-kw_only).
from dataclasses import dataclass, field, KW_ONLY


@dataclass
class Conn:
    host: str
    port: int = KW_ONLY
    timeout: float = field(default=1.0, kw_only=True)
    secret: str = field(default="", init=False, repr=False)


c = Conn("h", port=443, secret="s")
print(c.host, c.port, c.timeout)
print("secret" not in repr(c))
print(c.secret)


@dataclass
class WithInitFalse:
    x: int
    y: int = field(init=False)

    def __post_init__(self):
        self.y = self.x * 2


w = WithInitFalse(5)
print(w.x, w.y)
