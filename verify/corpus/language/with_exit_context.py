class CM:
    def __enter__(self):
        return self
    def __exit__(self, t, v, tb):
        xyzzy
try:
    with CM():
        1 / 0
except NameError as e:
    print(type(e.__context__).__name__)
