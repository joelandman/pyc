# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: __post_init__ mutation and field(default_factory=...).
from dataclasses import dataclass, field


@dataclass
class Row:
    values: list = field(default_factory=list)
    n: int = 0

    def __post_init__(self):
        self.n = len(self.values)
        self.values.append(self.n)


r = Row(values=[10, 20])
print(r.n)
print(r.values)
print(Row().values)
