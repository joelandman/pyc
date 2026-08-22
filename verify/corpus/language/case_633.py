# corpus case — ground truth is CPython at run time (CHARTER I5).
class E(ValueError):
    def __init__(self, m):
        super().__init__(m)
try:
    raise E('v')
except ValueError as e:
    print(type(e).__name__, e)
