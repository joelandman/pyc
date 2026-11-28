# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: a mini OOP program stitching many constructs together.
# (base + metaclass registry + context mgr + enum + comprehensions +
#  generator), output must match CPython.
from contextlib import contextmanager
from enum import Enum


class Status(Enum):
    NEW = 0
    DONE = 1


registry = []


class Meta(type):
    def __init__(cls, name, bases, ns):
        super().__init__(name, bases, ns)
        Meta.registry.append(name)


class Task(metaclass=Meta):
    def __init__(self, name):
        self.name = name
        self.status = Status.NEW

    def finish(self):
        self.status = Status.DONE


@contextmanager
def tracking(label):
    print(f"begin {label}")
    try:
        yield
    finally:
        print(f"end {label}")


def run_all(tasks):
    return [t.name for t in tasks if t.status == Status.DONE]


with tracking("batch"):
    tasks = [Task(f"task{i}") for i in range(3)]
    for t in tasks:
        t.finish()
    print(run_all(tasks))

print(registry)
