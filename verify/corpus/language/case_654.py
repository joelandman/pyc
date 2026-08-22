# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(x):
    return x.bit_length()
try:
    print(f(None))
except AttributeError as e:
    print(type(e).__name__)
def g(x):
    return x.nope()
try:
    print(g(None))
except AttributeError as e:
    print(type(e).__name__)
