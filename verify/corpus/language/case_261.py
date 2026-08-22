# corpus case — ground truth is CPython at run time (CHARTER I5).
funcs = [abs, str, len]
print(funcs[0](-10), funcs[1](99), funcs[2]([1, 2, 3]))
