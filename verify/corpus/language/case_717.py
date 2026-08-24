for v in ([1,2], (1,2), [1,2,3], [], "ab", b"ab", bytearray(b"ab"), [1]):
    match v:
        case []: r = "empty"
        case [a]: r = f"one {a!r}"
        case [a, b]: r = f"two {a!r},{b!r}"
        case [a, b, *rest]: r = f"many {a!r},{b!r} rest={rest!r}"
        case _: r = "not a sequence"
    print(type(v).__name__, repr(v), "->", r)
