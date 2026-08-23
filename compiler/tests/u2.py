try:
    a, b = 1, 2, 3
except ValueError as e:
    print("too many:", e)
try:
    a, b, c = [1, 2]
except ValueError as e:
    print("not enough:", e)
try:
    a, b = 5
except TypeError as e:
    print("not iterable:", e)
