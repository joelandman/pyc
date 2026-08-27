# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# A call inside a loop used to leak C stack. The alloca for the call's argument
# array was emitted IN THE LOOP BODY, and an alloca is not reclaimed until the
# function returns, so each iteration took another ~16 bytes:
#
#     RecursionError: Stack overflow (used 8148 kB) while calling a Python
#     object
#
# after roughly 500,000 calls. A loop with no call in it ran forever, which is
# what localised it. Any long-running compiled program that calls anything in a
# loop would eventually have died.
#
# This file was `recursive_dealloc_stack.py` in known-gaps, recorded as
# "C-stack exhaustion during the dealloc chain, mechanism not established".
# That diagnosis was WRONG, and instructively so: the deallocation was never
# the problem. The loop that BUILDS the million filters is a call in a loop,
# and by the time anything was deallocated the stack had already gone. I had
# measured the wrong end of the program.
import gc

c = 0
for _ in range(1000000):
    c += len(b"a call in a loop")
print("calls in a loop:", c)

n = 600000
i = filter(bool, range(n))
for _ in range(n):
    i = filter(bool, i)
del i
gc.collect()
print("survived recursive dealloc")
