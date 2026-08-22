# corpus case — ground truth is CPython at run time (CHARTER I5).
d={'a':1,'b':2,'c':3}
del d['b']
print('a' in d, 'b' in d, 'c' in d, len(d))
