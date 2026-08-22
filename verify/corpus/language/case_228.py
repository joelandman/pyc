# corpus case — ground truth is CPython at run time (CHARTER I5).
d={'a':1,'b':2,'c':3}
del d['a'], d['b']
print(len(d), 'c' in d)
