# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: yield inside try/finally and yield with raise inside generator.
def gen_finally():
    try:
        yield 1
        yield 2
    finally:
        yield "cleanup"


print(list(gen_finally()))


def raising_gen():
    try:
        yield 1
        raise ValueError("boom")
    except ValueError:
        yield "caught"
    else:
        yield "else"


print(list(raising_gen()))
