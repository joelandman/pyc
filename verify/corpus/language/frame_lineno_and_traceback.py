import sys
import traceback

def probe(n):
    a = 1
    b = 2
    if n > 0:
        c = a + b
    else:
        c = a - b
    d = sys._getframe().f_lineno
    return c, d

def fail_at(n):
    x = 10
    y = 20
    z = x + y
    if n:
        raise ArithmeticError("first")
    else:
        raise LookupError("second")

out = []
out.append(probe(1))
out.append(probe(0))
for flag in (1, 0):
    try:
        fail_at(flag)
    except Exception as e:
        out.append("".join(traceback.format_exception(type(e), e, e.__traceback__)))
print(out)
