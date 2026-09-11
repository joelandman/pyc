class F:
    def __call__(self, *a):
        return a
f = F()
args = tuple(range(5))
print(f(*args) is args)
