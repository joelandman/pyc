# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: frozen dataclass immutability and field(compare=False).
from dataclasses import dataclass, field


@dataclass(frozen=True)
class Conf:
    host: str
    port: int = 80
    secret: str = field(default="", compare=False)


c = Conf("h", 443, secret="s")
print(c)
print(c.host, c.port)
print(c == Conf("h", 443))
try:
    c.host = "x"
    print("MUTABLE")
except Exception as e:
    print(type(e).__name__)
