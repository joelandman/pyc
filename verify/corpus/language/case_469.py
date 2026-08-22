# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    raise KeyError("k")
except ValueError:
    print("wrong")
except KeyError:
    print("ke")
try:
    try:
        raise KeyError("inner")
    except ValueError:
        print("no")
except KeyError:
    print("outer")
