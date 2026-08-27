# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# The cases that distinguish a real unwind from a single hard-coded cleanup:
# two managers on one statement, a with inside a with, a with inside a
# try/finally, a break targeting a loop INSIDE the body (which must NOT run
# __exit__ first), and an exception still reaching its handler.
class CM:
    def __init__(self, tag):
        self.tag = tag

    def __enter__(self):
        print(f"enter {self.tag}")
        return self.tag

    def __exit__(self, *a):
        print(f"exit {self.tag}")
        return False


def multi():
    with CM("A"), CM("B"):
        return "multi"


def deep():
    with CM("outer"):
        with CM("inner"):
            return "deep"


def inner_loop():
    with CM("L"):
        for i in range(4):
            if i == 2:
                break
        return f"loop stopped at {i}"


def with_in_try():
    try:
        with CM("t"):
            return "from-with"
    finally:
        print("finally ran")


def raises():
    try:
        with CM("x"):
            raise ValueError("boom")
    except ValueError as e:
        return f"caught {e}"


print(multi())
print(deep())
print(inner_loop())
print(with_in_try())
print(raises())
