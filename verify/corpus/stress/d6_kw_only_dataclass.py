# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: kw_only dataclass fields, field ordering constraints.
from dataclasses import dataclass, field


@dataclass
class Conn:
    host: str
    port: int = field(kw_only=True)
    timeout: float = field(kw_only=True, default=1.0)


c = Conn("h", port=80, timeout=2.5)
print(c.host, c.port, c.timeout)
print(c.host, 80, 2.5)
