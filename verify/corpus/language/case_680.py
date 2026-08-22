# corpus case — ground truth is CPython at run time (CHARTER I5).
class C057:
    def f(self):
        s = super()
        try:
            print(repr(s[1:3]))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(repr(tuple(s)))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(list(map(str, s)))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(list(filter(None, s)))
        except TypeError as e:
            print(type(e).__name__)
C057().f()
try:
    None[1:3] = [1]
    print('setslice-ok')
except TypeError as e:
    print(type(e).__name__)
try:
    None[0] = 1
    print('setitem-ok')
except TypeError as e:
    print(type(e).__name__)
try:
    print('{0.x}'.format({'x': None}))
except AttributeError as e:
    print(type(e).__name__)
try:
    print('abc'.rindex(1))
except TypeError as e:
    print(type(e).__name__)
print("banana".find("", 2, 2))
print("banana".find("", 6, 6))
print("banana".rfind("", 2, 2))
print("banana".find("a", 0, -1))
print(repr('a\x00b'))
print(len('a\x00'+'b'))
print('a\x00b')
def add(a, b):
    return a + b
try:
    print(add(None, 1))
except TypeError as e:
    print(type(e).__name__)
try:
    print(add([1], 2))
except TypeError as e:
    print(type(e).__name__)
try:
    print(add(1, None))
except TypeError as e:
    print(type(e).__name__)
try:
    print(1 - [1])
except TypeError as e:
    print(type(e).__name__)
