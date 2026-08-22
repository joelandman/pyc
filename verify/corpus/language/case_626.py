# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    def call(self, x):
        return "user-call:" + str(x)
    def exists(self):
        return "user-exists"
    def bit_length(self):
        return 99
    def fromkeys(self, k):
        return "user-fromkeys"
    def unlink(self):
        return "user-unlink"
    def isfile(self):
        return "user-isfile"
    def isdir(self):
        return "user-isdir"
    def check_output(self, x):
        return "user-co"

c = C()
print(c.call(1), c.exists(), c.bit_length(), c.fromkeys([1]))
print(c.unlink(), c.isfile(), c.isdir(), c.check_output("x"))
