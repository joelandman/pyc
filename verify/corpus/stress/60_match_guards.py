# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: match with value patterns, if guards, capture as.
def classify(n):
    match n:
        case 0:
            return "zero"
        case 1 | 2 | 3:
            return "small"
        case x if x > 100:
            return "big"
        case x if x < 0:
            return "neg"
        case _:
            return "mid"


print(classify(0))
print(classify(2))
print(classify(150))
print(classify(-5))
print(classify(42))
