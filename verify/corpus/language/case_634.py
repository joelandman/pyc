# corpus case — ground truth is CPython at run time (CHARTER I5).
class E(Exception):
    def __init__(self, a, b):
        super().__init__(a, b)
e = E('a', 'b')
print(e.args[0], e.args[1])
