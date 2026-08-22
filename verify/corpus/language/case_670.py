# corpus case — ground truth is CPython at run time (CHARTER I5).
class E(Exception):
    def __init__(self, m):
        super().__init__(m)
    def __str__(self):
        return 'wrap:' + super().__str__()
print(E('boom'))
print(E('boom').__str__())
