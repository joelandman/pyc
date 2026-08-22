# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    raise ValueError("msg here")
except ValueError as e:
    print("got:", e)
try:
    raise ValueError("x")
except ValueError:
    print("h")
finally:
    print("fin")
try:
    print("ok")
except ValueError:
    print("no")
else:
    print("else ran")
