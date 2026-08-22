# corpus case — ground truth is CPython at run time (CHARTER I5).
def apply(f, x): return f(x)
print(apply(abs, -5), apply(str, 42), apply(len, 'hello'))
