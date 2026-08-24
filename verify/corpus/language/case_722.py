for v in (1, 2, 3, "x"):
    match v:
        case 1 | 2 | 3 as n: print(v, "-> small", n)
        case other: print(v, "-> other", other)
for v in ([1,2], [3,4]):
    match v:
        case [1, _] | [3, _] as whole: print(v, "-> ok", whole)
