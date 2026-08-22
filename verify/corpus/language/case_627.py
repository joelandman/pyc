# corpus case — ground truth is CPython at run time (CHARTER I5).
class C:
    def bit_length(self):
        return 99
    def call(self, x):
        return "U"
    def fromkeys(self, k):
        return "F"
    def exists(self):
        return "E"

def f_bl(o):
    return o.bit_length()
def f_call(o):
    return o.call(1)
def f_fk(o):
    return o.fromkeys([1])
def f_ex(o):
    return o.exists()

c = C()
print(f_bl(c), f_bl(7), f_call(c), f_fk(c), f_ex(c))
print((7).bit_length(), dict.fromkeys([1, 2], 0))
def f_dfk(d):
    return d.fromkeys(["a"], 1)
print(f_dfk({}))
