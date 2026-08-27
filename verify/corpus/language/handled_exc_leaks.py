# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# An exception leaving an except handler must pop that handler's exc_info.
# return/break/continue did so via pop_open_handlers; the UNWIND path did not,
# because the landing pad already dropped the reference and that looked like
# enough. Dropping a reference is not popping a stack: CPython's "currently
# handled exception" stayed set for the rest of the program, so every later
# raise inherited a __context__ from an exception handled long before, and its
# traceback claimed "During handling of the above exception..." about something
# entirely unrelated.
try:
    try:
        raise ValueError("inner")
    except ValueError:
        raise TypeError("outer")        # propagates OUT of the handler
except TypeError:
    pass

try:
    raise KeyError("later")
except KeyError as e:
    print("after unwinding out of a handler:", repr(e.__context__))


def via_return():
    try:
        raise ValueError("v")
    except ValueError:
        return "returned"


via_return()
try:
    raise KeyError("after return")
except KeyError as e:
    print("after returning from a handler:", repr(e.__context__))


def via_break():
    for _ in range(1):
        try:
            raise ValueError("v")
        except ValueError:
            break
    return "broke"


via_break()
try:
    raise KeyError("after break")
except KeyError as e:
    print("after breaking out of a handler:", repr(e.__context__))

# A genuine context must still be recorded.
try:
    try:
        raise ValueError("real inner")
    except ValueError:
        raise TypeError("real outer")
except TypeError as e:
    print("genuine context preserved:", repr(e.__context__))
