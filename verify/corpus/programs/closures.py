#!/usr/bin/env python3


# Basic closure: captures a single outer variable
def make_counter():
    n = 0

    def inner():
        nonlocal n
        n = n + 1
        return n

    return inner


c1 = make_counter()
c2 = make_counter()

print(c1())   # 1
print(c1())   # 2
print(c1())   # 3
print(c2())   # 1  (independent counter)
print(c1())   # 4


# Closure capturing multiple variables
def make_adder(base, step):
    def inner():
        nonlocal base
        base = base + step
        return base

    return inner


by2 = make_adder(10, 2)
by5 = make_adder(0, 5)

print(by2())   # 12
print(by2())   # 14
print(by5())   # 5
print(by5())   # 10

# Closure over loop variable
funcs = []
i = 1
while i <= 3:
    val = i
    funcs.append(lambda val=val: val * 2)
    i = i + 1
print(funcs[0]())   # 2
print(funcs[1]())   # 4
print(funcs[2]())   # 6
