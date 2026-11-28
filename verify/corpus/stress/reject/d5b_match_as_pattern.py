# corpus case — ground truth is CPython at run time (CHARTER I5).
# BOUNDARY probe. `case int() as v` — an `as`-binding on a class pattern is
# valid CPython but pyc's match parser rejects it with "invalid syntax" at the
# `as` token. Records where the match parser's coverage ends. The intent of
# this case is DID_NOT_COMPILE; if it ever compiles, the boundary has moved.
match 5:
    case int() as v if v > 100:
        print("big")
