# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: ExitStack composing multiple managers and ordered __exit__.
from contextlib import ExitStack, contextmanager


log = []


@contextmanager
def step(name):
    log.append(f"begin {name}")
    try:
        yield
    finally:
        log.append(f"end {name}")


with ExitStack() as stack:
    stack.enter_context(step("a"))
    stack.enter_context(step("b"))
    log.append("inside")

print(log)
