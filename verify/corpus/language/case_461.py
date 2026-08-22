# corpus case — ground truth is CPython at run time (CHARTER I5).
a=0
b=0
def outer():
    a=1
    b=2
    def swap():
        nonlocal a,b
        a,b = b,a
    swap()
    print(a,b)
outer()
print(a,b)
