#!/usr/bin/env python3

# if modifier
if 1:
    print("positive")
if 0:
    print("never")

# unless modifier
if not 0:
    print("ok")
if not 1:
    print("never2")

# while modifier
i = 0
while i < 3:
    i += 1
print(i)   # 3

# until modifier
j = 3
while not (j <= 0):
    j -= 1
print(j)   # 0

# for modifier
for _v in (1, 2, 3):
    print(_v)

arr = ["a", "b", "c"]
for _v in arr:
    print(_v)


# return with modifier
def check_pos(n):
    if n < 0:
        return 0
    return n


print(check_pos(5))    # 5
print(check_pos(-1))   # 0

# push with modifier
out = []
x = 7
if x > 5:
    out.append(x)
print(out[0])   # 7

# last/next with modifier
for v in (1, 2, 3, 4, 5):
    if v == 3:
        continue
    if v == 5:
        break
    print(v)    # 1 2 4
