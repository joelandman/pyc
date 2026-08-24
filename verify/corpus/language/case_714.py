for v in (1, 2, "a", 3.0, True, None):
    match v:
        case 1: print(v, "-> one")
        case "a": print(v, "-> letter a")
        case 3.0: print(v, "-> three")
        case _: print(v, "-> other")
