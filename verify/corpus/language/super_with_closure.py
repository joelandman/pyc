class A:
    def f(self):
        return 'A'
class E(A):
    def f(self):
        def nested():
            self
        return super().f() + 'E'
print(E().f())
