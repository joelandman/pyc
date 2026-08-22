# corpus case — ground truth is CPython at run time (CHARTER I5).
fns = [lambda x: x+1, lambda x: x*2]
print((lambda y: y*y)(3), fns[0](10), fns[1](7))
