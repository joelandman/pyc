#!/usr/bin/env python3


def fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)


i = 0
while i < 10:
    print(fib(i))
    i += 1
