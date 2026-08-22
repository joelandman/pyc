# corpus case — ground truth is CPython at run time (CHARTER I5).
print("banana".count("a", 2, 3))
try:
    print("banana".index("a", 2, 3))
except ValueError as e:
    print(type(e).__name__)
print("banana".startswith("n", 2, 3))
print("banana".endswith("n", 0, 3))
print("banana".count("a"))
print("banana".index("a"))
print("banana".startswith("ba"))
print("banana".endswith("na"))
def f(s):
    return (s.count("a", 2, 3), s.startswith("n", 2, 3), s.endswith("n", 0, 3))
print(f("banana"))
try:
    print("banana".index("a", 2, 3))
except ValueError:
    print('idx')
