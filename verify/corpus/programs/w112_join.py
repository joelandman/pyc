# W12.4 leftover / I-112: native join of speculative call into i64 local.
def add(a, b):
    return a + b

def loop(x):
    s = 0
    for i in range(4):
        s = add(s, x)
    return s

print(loop(1))
print(loop(2))
print(add(1, 2))
