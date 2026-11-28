# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: suppress swallowing an exception, else the block runs.
from contextlib import suppress


saw = []
with suppress(ZeroDivisionError):
    1 / 0
    saw.append("div")
print(saw)

with suppress(KeyError):
    print("missing-block")

total = 0
with suppress(ValueError):
    total = sum(int(x) for x in [])
print(total)
