def f():
    try:
        return 1
    finally:
        print("fin1")
print(f())
def g():
    try:
        raise ValueError("deep")
    finally:
        print("g fin")
try:
    g()
except ValueError as e:
    print("main:", e)
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
def h():
    try:
        try:
            return "inner"
        finally:
            print("f1")
    finally:
        print("f2")
print(h())
try:
    print("ok")
finally:
    print("f3")
