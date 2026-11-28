# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: f-string debug (x=), format spec nesting, replacement fields.
def show():
    val = 42
    name = "pi"
    print(f"{name}={val}")
    print(f"{val:05x}")
    print(f"{22/7:.4f}")
    items = [1, 2, 3]
    print(f"{len(items)} items")
    print(f"{items!r}")
    nested = f"{f'{val}'!s:>3}"
    print(nested)


show()
