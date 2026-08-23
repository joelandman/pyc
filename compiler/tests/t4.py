def f():
    try:
        return "from try"
    except Exception:
        return "handler"
print(f())
try:
    d = {}
    print(d["missing"])
except KeyError as e:
    print("key error:", e)
