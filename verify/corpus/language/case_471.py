# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    print(1 // 0)
except ZeroDivisionError:
    print("zde")
lst = [1, 2]
try:
    print(lst[5])
except LookupError:
    print("ie")
d = {"a": 1}
try:
    print(d["zz"])
except KeyError as e:
    print("ke:", e)
try:
    print(int("nope"))
except ValueError:
    print("ve")
try:
    print(5 // 0)
except ArithmeticError:
    print("arith")
