# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# __exit__ must run on every way out of a `with`, including return, break and
# continue. The nested-return shape is the one that mattered: the old guard
# scanned only the body's DIRECT children, so a return one level down inside an
# `if` compiled, skipped __exit__, and exited 0 with the wrong output.
class CM:
    def __init__(self, tag):
        self.tag = tag

    def __enter__(self):
        print(f"enter {self.tag}")
        return self.tag

    def __exit__(self, *a):
        print(f"exit {self.tag}")
        return False


def direct():
    with CM("d"):
        return "R"


def nested_if(c):
    with CM("n"):
        if c:
            return "early"
    return "normal"


def brk(items):
    out = []
    for i in items:
        with CM(f"b{i}"):
            if i == 2:
                break
            out.append(i)
    return out


def cont(items):
    out = []
    for i in items:
        with CM(f"c{i}"):
            if i == 2:
                continue
            out.append(i)
    return out


print(direct())
print(nested_if(True))
print(nested_if(False))
print(brk([1, 2, 3]))
print(cont([1, 2, 3]))
