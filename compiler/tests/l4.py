xs = [1, 2, 3]
ys = [x for x in xs]
print(ys)
try:
    print(x)
except NameError as e:
    print("comprehension variable did not leak")
