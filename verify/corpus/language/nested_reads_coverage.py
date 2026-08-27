# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# A name is a CELL variable of a function exactly when something nested reads
# it, so the walk that finds nested reads decides which names get cells. Ten of
# its arms were incomplete, and every omission had the same consequence: a
# generator or coroutine compiled by CPython came back with a freevar that pyc
# had no cell to bind, and the whole file was refused.
#
# Each function below reads an enclosing local from a position that used to be
# skipped.
def except_type():
    class Boom(Exception):
        pass

    def gen():
        try:
            yield "a"
        except Boom:                 # the caught TYPE was never walked
            yield "caught"

    g = gen()
    out = [next(g), g.throw(Boom("x"))]
    return out


def with_target():
    import contextlib

    @contextlib.contextmanager
    def cm():
        yield "value"

    def gen():
        with cm() as bound:          # the as-target was never walked
            yield bound

    return list(gen())


def raise_cause():
    class Cause(Exception):
        pass

    def gen():
        try:
            raise ValueError("v") from Cause("c")
        except ValueError as e:      # `from` cause was never walked
            yield type(e.__cause__).__name__

    return list(gen())


def assert_msg():
    note = "note"

    def gen():
        assert True, note            # the assert MESSAGE was never walked
        yield note

    return list(gen())


def match_guard():
    limit = 2

    def gen():
        match 3:
            case x if x > limit:     # the GUARD was never walked
                yield "over"
            case _:
                yield "under"

    return list(gen())


print("except type:", except_type())
print("with target:", with_target())
print("raise cause:", raise_cause())
print("assert message:", assert_msg())
print("match guard:", match_guard())
