class Base:
    def greet(self): return 'base'
class Mid(Base):
    def greet(self): return 'mid>' + super().greet()
class Leaf(Mid):
    def greet(self): return 'leaf>' + super().greet()
print(Leaf().greet())
class E(Exception):
    def __init__(self, m):
        super().__init__(m)
    def __str__(self): return 'wrap:' + super().__str__()
try: raise E('boom')
except E as e: print(e)
class F(Base):
    def greet(self): return 'exp>' + super(F, self).greet()
print(F().greet())
class G(Base):
    def greet(self):
        s = super()
        return 'v>' + s.greet()
print(G().greet())
def outer():
    class H(Base):
        def greet(self): return 'nest>' + super().greet()
    return H().greet()
print(outer())
# super() inside a lambda is covered by n_super_errors.py, where the
# exception is caught: uncaught, the diff is pyc's missing traceback,
# not the super() behaviour under test.
