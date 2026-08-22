# corpus case — ground truth is CPython at run time (CHARTER I5).
def f(x): return x+1
r=0
for i in range(3):
    r = r + f(i)
print(r)
