# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: match sequence patterns with *rest capture.
def shape(seq):
    match seq:
        case []:
            return "empty"
        case [first]:
            return f"one {first}"
        case [first, *rest]:
            return f"first {first} rest {rest}"
        case _:
            return "other"


print(shape([]))
print(shape([7]))
print(shape([1, 2, 3, 4]))
print(shape((9, 8, 7)))
