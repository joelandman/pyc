#!/usr/bin/env python3
from functools import cmp_to_key


def spaceship(a, b):
    return (a > b) - (a < b)


def perl_oct(s):
    s = s.strip()
    if s[:2] in ("0x", "0X"):
        return int(s, 16)
    if s[:2] in ("0b", "0B"):
        return int(s, 2)
    return int(s or "0", 8)


# --- math ---
print(abs(-5))               # 5
print(abs(3.7))              # 3.7
print(int(3.9))              # 3
print(int(-3.9))             # -3
print("%.1f" % (16 ** 0.5))  # 4.0

# --- case ---
print("hello".upper())       # HELLO
print("WORLD".lower())       # world
print("foo"[0].upper() + "foo"[1:])  # Foo
print("BAR"[0].lower() + "BAR"[1:])  # bAR

# --- index / rindex ---
print("hello world".find("world"))      # 6
print("hello world".find("xyz"))        # -1
print("abcabc".find("b", 2))            # 4
print("hello world".rfind("l"))         # 9
print("hello world".rfind("l", 0, 5 + 1))  # 3

# --- chr / ord ---
print(chr(65))      # A
print(ord("A"))     # 65
print(ord("abc"[0]))  # 97

# --- hex / oct ---
print(int("ff", 16))     # 255
print(int("0xFF", 16))   # 255
print(perl_oct("077"))   # 63
print(perl_oct("0b1010"))  # 10

# --- reverse ---
arr = [1, 2, 3, 4, 5]
rev = list(reversed(arr))
print(",".join(str(v) for v in rev))   # 5,4,3,2,1
print("hello"[::-1])                    # olleh

# --- map ---
nums = [1, 2, 3, 4, 5]
doubled = [n * 2 for n in nums]
print(",".join(str(v) for v in doubled))  # 2,4,6,8,10
strs = [f"n{n}" for n in nums]
print(strs[0])   # n1
print(strs[4])   # n5

# --- grep ---
evens = [n for n in nums if n % 2 == 0]
print(",".join(str(v) for v in evens))  # 2,4
big = [n for n in nums if n > 3]
print(",".join(str(v) for v in big))    # 4,5

# --- sort with comparator ---
sn = sorted([5, 3, 1, 4, 2], key=cmp_to_key(lambda a, b: spaceship(a, b)))
print(",".join(str(v) for v in sn))   # 1,2,3,4,5
sr = sorted(nums, key=cmp_to_key(lambda a, b: spaceship(b, a)))
print(",".join(str(v) for v in sr))   # 5,4,3,2,1
words = ["banana", "apple", "cherry"]
sw = sorted(words, key=cmp_to_key(lambda a, b: spaceship(a, b)))
print(",".join(sw))    # apple,banana,cherry
swr = sorted(words, key=cmp_to_key(lambda a, b: spaceship(b, a)))
print(",".join(swr))   # cherry,banana,apple

# --- <=> and cmp ---
print(spaceship(1, 2))      # -1
print(spaceship(2, 2))      # 0
print(spaceship(3, 2))      # 1
print(spaceship("a", "b"))  # -1
print(spaceship("b", "b"))  # 0
print(spaceship("c", "b"))  # 1

print("done")
