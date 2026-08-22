# corpus case — ground truth is CPython at run time (CHARTER I5).
print("banana".find("a", 2, 3))
print("banana".find("a", 2, 6))
def f(s):
    return s.find("a", 2, 3)
print(f("banana"))
print("banana".rfind("n", 0, 3))
