# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    try:
        raise ValueError("a")
    except ValueError:
        print("h")
        raise KeyError("b")
    finally:
        print("fin")
except KeyError as e:
    print("outer", e)
try:
    try:
        pass
    except ValueError:
        print("no")
    else:
        raise KeyError("e")
    finally:
        print("fin2")
except KeyError:
    print("outer2")
