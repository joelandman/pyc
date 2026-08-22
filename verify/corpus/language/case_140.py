# corpus case — ground truth is CPython at run time (CHARTER I5).
x=0
def f():
    global x
    x=1
f()
print(x)
