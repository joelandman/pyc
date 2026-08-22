# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    def __init__(self):
        super().__init__()
        self.ok = 1
print(C().ok)
