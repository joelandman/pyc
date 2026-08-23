class CM:
    def __enter__(self):
        print("enter")
        return "value"
    def __exit__(self, t, v, tb):
        print("exit", t is None)
        return False
with CM() as x:
    print("body", x)
print("after")
