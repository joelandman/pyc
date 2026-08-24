class C:
    A = 1
    B = 2
for v in (1, 2, 3):
    match v:
        case C.A: print(v, "-> A")
        case C.B: print(v, "-> B")
        case _: print(v, "-> neither")
