# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: abstractmethod override in a chain, ABCMeta metaclass dispatch.
from abc import ABC, abstractmethod


class Base(ABC):
    @abstractmethod
    def compute(self) -> int:
        ...


class Mid(Base):
    def compute(self) -> int:
        return 42


class Leaf(Mid):
    pass


l = Leaf()
print(l.compute())
print(isinstance(l, Base))
