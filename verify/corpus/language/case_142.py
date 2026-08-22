# corpus case — ground truth is CPython at run time (CHARTER I5).
x=42
def f():
    global x
    return x
print(f())
