# corpus case — ground truth is CPython at run time (CHARTER I5).
d = {'inc': lambda x: x+1, 'dbl': lambda x: x*2}
print(d['inc'](5), d['dbl'](7))
