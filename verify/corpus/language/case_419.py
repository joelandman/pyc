# corpus case — ground truth is CPython at run time (CHARTER I5).
def call_it(fn, v):
    return fn(v)
print(call_it(lambda x: x*x, 6))
fns=[lambda y:y+10, lambda y:y*2]
print(fns[0](1), fns[1](7))
