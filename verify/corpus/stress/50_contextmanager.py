# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: @contextmanager generator-based context manager + finally cleanup.
from contextlib import contextmanager


events = []


@contextmanager
def tracked(tag):
    events.append(f"enter:{tag}")
    try:
        yield tag
    finally:
        events.append(f"exit:{tag}")


with tracked("a") as x:
    events.append(f"body:{x}")

print(events)
