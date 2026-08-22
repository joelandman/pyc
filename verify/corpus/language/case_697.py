# corpus case — ground truth is CPython at run time (CHARTER I5).
print(list(zip("ab", "cd")))
print(list(zip("ab", "cd", "ef")))
print(list(zip("ab", [1, 2])))
print(list(zip("ab", "c")))
print(list(zip(b"ab", [1, 2])))
try:
    print(list(zip(None, [1])))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(zip(1, [1])))
except TypeError as e:
    print(type(e).__name__)
