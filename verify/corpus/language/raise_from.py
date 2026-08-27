# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# `raise X from Y` sets __cause__ AND __suppress_context__. Measured against
# 3.14.7, and each line below is one of those observations:
#
#   from an instance   __cause__ = it,            suppress = True
#   from a CLASS       __cause__ = Class(),       suppress = True  (instantiated)
#   from None          __cause__ = None,          suppress = True
#   no `from`          __cause__ = None,          suppress = False
#   from a non-exception -> TypeError, and it REPLACES the requested exception
#
# `from None` is the row worth reading twice: it does not merely leave
# __cause__ unset, it suppresses the implicit context, which is the whole
# reason to write it. __context__ is still recorded either way.
def show(tag, e):
    print(f"{tag}: cause={e.__cause__!r} "
          f"ctx={type(e.__context__).__name__} suppress={e.__suppress_context__}")


try:
    try:
        raise ValueError("inner")
    except ValueError as v:
        raise TypeError("outer") from v
except TypeError as e:
    show("from instance", e)

try:
    raise TypeError("t") from ValueError
except TypeError as e:
    show("from class", e)

try:
    try:
        raise ValueError("inner")
    except ValueError:
        raise TypeError("outer") from None
except TypeError as e:
    show("from None", e)

try:
    try:
        raise ValueError("inner")
    except ValueError:
        raise TypeError("outer")
except TypeError as e:
    show("plain", e)

try:
    raise TypeError("t") from 42
except TypeError as e:
    print("non-exception cause ->", type(e).__name__, e)

try:
    raise ValueError from KeyError("k")
except ValueError as e:
    show("class raised", e)


def mk(tag):
    print("  eval", tag)
    return ValueError(tag)


try:
    raise mk("exc") from mk("cause")     # exception expression evaluates first
except ValueError as e:
    show("order", e)

try:
    raise TypeError("t") from (1 / 0)
except ZeroDivisionError as e:
    print("raising cause expression wins:", e)
