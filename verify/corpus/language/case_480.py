# corpus case — ground truth is CPython at run time (CHARTER I5).
exc = ValueError
e = exc("hello world")
print(e)
MyError = ValueError
try:
    raise MyError("first")
except ValueError:
    print("caught 1")
try:
    raise MyError("second")
except ValueError:
    print("caught 2")
exc2 = ZeroDivisionError
try:
    raise exc2("can't divide")
except ArithmeticError:
    print("caught ArithmeticError")
exc3 = KeyError
try:
    raise exc3("missing")
except KeyError:
    print("caught KeyError")
