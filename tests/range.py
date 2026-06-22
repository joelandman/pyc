#!/usr/bin/env python3

# basic foreach with range
for i in range(1, 6):
    print(f"{i} ", end="")
print()                              # 1 2 3 4 5

# for modifier with range
for x in range(1, 4):
    print(x)                         # 1 2 3

# array assignment from range
r = list(range(1, 6))
print(len(r))                        # 5
print(r[0])                          # 1
print(r[4])                          # 5

# range in expression context
a = list(range(10, 16))
print(", ".join(str(v) for v in a))  # 10, 11, 12, 13, 14, 15

# range with variables
lo = 3
hi = 7
b = list(range(lo, hi + 1))
print(len(b))                        # 5
print(b[0])                          # 3
print(b[4])                          # 7

# empty range (lo > hi)
empty = list(range(5, 3 + 1))        # 5..3 is empty in Perl
print(len(empty))                    # 0

# range used in join
print("-".join(str(v) for v in range(1, 5)))  # 1-2-3-4

# C-style for with range variable
total = 0
for n in range(1, 11):
    total += n
print(total)                         # 55
