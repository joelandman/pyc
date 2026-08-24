match 99:
    case 1: print("one")
    case 2: print("two")
print("fell through, no error")
def f(v):
    match v:
        case 1: return "one"
    return "none matched"
print(f(1), f(2))
