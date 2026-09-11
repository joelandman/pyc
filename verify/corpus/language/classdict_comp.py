class _C:
    res = [(lambda: __classdict__)() for _ in [1]]
print("res" in _C.res[0])
