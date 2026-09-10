class X:
    def f(x):
        nonlocal __class__
        del __class__
        super()
try:
    X().f()
except RuntimeError as e:
    print("empty" in str(e))
