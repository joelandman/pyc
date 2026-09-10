def f(x):
    del x
    super()
try:
    f(None)
except RuntimeError as e:
    print("deleted" in str(e))
