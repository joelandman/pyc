#!/usr/bin/env python3
import sys


def fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)


N = int(sys.argv[1])
for i in range(0, N + 1):
    print("%i\t%li" % (i, fib(i)))
    i += 1  # note: this $i is local to the loop body and is NOT the loop
    #         counter (matches Perl's foreach semantics)
