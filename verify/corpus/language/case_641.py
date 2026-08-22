# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    def __init__(self):
        self.x = 9
print('{0.x}'.format(C()))
