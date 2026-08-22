# corpus case — ground truth is CPython at run time (CHARTER I5).
exc = ValueError
print(exc is ValueError)
print(ValueError is exc)
exc2 = KeyError
print(ValueError is exc2)
