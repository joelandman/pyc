# W9.19 / I-210: any/all/sum/zip/enumerate/min/max of complex/function.
try:
    print(any(1 + 2j))
except TypeError as e:
    print(type(e).__name__)
try:
    print(all(1 + 2j))
except TypeError as e:
    print(type(e).__name__)
def f():
    return 0
try:
    print(sum(f))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(zip(1 + 2j, [1])))
except TypeError as e:
    print(type(e).__name__)
try:
    print(list(enumerate(1 + 2j)))
except TypeError as e:
    print(type(e).__name__)
try:
    print(min(1 + 2j))
except TypeError as e:
    print(type(e).__name__)
try:
    print(max(f))
except TypeError as e:
    print(type(e).__name__)
print(any([0, 1]))
print(sum([1, 2]))
print(list(zip([1], [2])))
print(list(map(str, [1])))
try:
    print(list(map(str, 1 + 2j)))
except TypeError as e:
    print(type(e).__name__)
