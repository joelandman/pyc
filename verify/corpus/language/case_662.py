# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    def is_file(self):
        return "user-file"
    def isoformat(self):
        return "user-iso"
    def group(self, n):
        return "user-group"
    def is_integer(self):
        return "user-int"
    def most_common(self):
        return "user-mc"
    def format(self, a=0):
        return "user-fmt:" + str(a)
print(C().is_file())
print(C().isoformat())
print(C().group(1))
print(C().is_integer())
print(C().most_common())
print(C().format(a=1))
