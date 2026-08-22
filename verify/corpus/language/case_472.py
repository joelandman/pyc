# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    try:
        raise ValueError("original")
    except ValueError as e:
        print("inner:", e)
        raise
except ValueError as e2:
    print("outer:", e2)
try:
    raise TypeError("t")
except (ValueError, TypeError) as e:
    print("tuple:", e)
def g():
    try:
        raise ValueError("deep")
    finally:
        print("g fin")
try:
    g()
except ValueError as e:
    print("main:", e)
