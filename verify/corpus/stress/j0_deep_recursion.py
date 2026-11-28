# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: deep recursion / C-stack. Both sides must raise RecursionError on the
# same input and handle the same depth. Identical crash (both exit 1 with
# RecursionError) counts as a pass; a divergence is the finding.
import sys


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


print(fib(15))


def deep(k):
    return deep(k - 1) + 1


# Prove a bounded deep recursion works end to end.
print(deep(2000))


# Beyond that it blows the C stack; both sides must RecursionError here.
try:
    deep(20000)
    print("no-error")
except RecursionError:
    print("RecursionError")

# sys.getrecursionlimit reports the same bound on both sides.
print(sys.getrecursionlimit())
