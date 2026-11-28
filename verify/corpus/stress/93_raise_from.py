# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: raise ... from chaining propagates __cause__ and __context__.
def first():
    try:
        1 / 0
    except ZeroDivisionError as e:
        try:
            raise RuntimeError("wrapped") from e
        except RuntimeError as e2:
            return (str(e2), str(e2.__cause__))


print(first())


def second():
    try:
        raise KeyError("orig")
    except KeyError:
        try:
            raise ValueError("new")
        except ValueError as e2:
            return type(e2.__context__).__name__


print(second())
