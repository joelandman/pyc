# corpus case — ground truth is CPython at run time (CHARTER I5).
class AlwaysEqual:
    def __eq__(self, other):
        return True
a = AlwaysEqual()
print(a == 5, a == 'anything')
