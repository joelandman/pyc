for v in ({"a":1}, {"a":1,"b":2}, {"b":2}, {}, [1]):
    match v:
        case {"a": x, **rest}: print(v, "-> a=", x, "rest=", rest)
        case {}: print(v, "-> some mapping")
        case _: print(v, "-> not a mapping")
