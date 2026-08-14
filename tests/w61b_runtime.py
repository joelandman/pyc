# W6.1b: leftover wrong-answers I-121–I-125 (same Runtime lock).
class C121:
    def f(self):
        s = super()
        try:
            print(sorted(s))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(list(reversed(s)))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(any(s))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(all(s))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(sum(s))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(list(enumerate(s)))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(list(zip(s, [1])))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(min(s))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(bool(s))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(s + (1,))
        except TypeError as e:
            print(type(e).__name__)
        try:
            print(s * 2)
        except TypeError as e:
            print(type(e).__name__)
C121().f()
try:
    1[0] = 2
    print("int-ok")
except TypeError as e:
    print(type(e).__name__)
try:
    True[0] = 1
    print("bool-ok")
except TypeError as e:
    print(type(e).__name__)
try:
    {1}[0] = 2
    print("set-ok")
except TypeError as e:
    print(type(e).__name__)
print("banana".find("n", -3))
print("a\x00b".upper())
print("".join(["a\x00", "b"]))
def div(a, b):
    return a // b
try:
    print(div(1, [1]))
except TypeError as e:
    print(type(e).__name__)
try:
    print(1 / None)
except TypeError as e:
    print(type(e).__name__)
try:
    print(1 % [1])
except TypeError as e:
    print(type(e).__name__)
try:
    print(pow(None, 1))
except TypeError as e:
    print(type(e).__name__)
try:
    print(-None)
except TypeError as e:
    print(type(e).__name__)
try:
    print(None << 1)
except TypeError as e:
    print(type(e).__name__)
try:
    print([1] & 2)
except TypeError as e:
    print(type(e).__name__)
print(1 // 2)
print(1 << 2)
print({1} | {2})
