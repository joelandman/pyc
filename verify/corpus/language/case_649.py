# corpus case — ground truth is CPython at run time (CHARTER I5).
class E(Exception):
    def __init__(self, m):
        print(super().__init__(m))
E('x')
