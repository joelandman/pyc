for v in ([1,2,3,4], [1], [1,2]):
    match v:
        case [first, *mid, last]: print(v, "->", first, mid, last)
        case [only]: print(v, "-> only", only)
        case _: print(v, "-> none")
