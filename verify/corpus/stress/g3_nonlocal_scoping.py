# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: nonlocal scoping edges — rebinding nested scopes, nonlocal in loops,
# nonlocal in comprehension-closing scope, and closure capture of the same name.
def make():
    count = 0
    def inc():
        nonlocal count
        count += 1
        return count
    return inc


f = make()
print(f())
print(f())
print(f())


def outer():
    x = "outer"
    def inner():
        return x
    def inner2():
        nonlocal x
        x = "inner2"
        return x
    return inner, inner2


i1, i2 = outer()
print(i1())
print(i2())
print(i1())


# comprehension loop var after loop: does a later shadow see the loop value?
def comp_scope():
    values = []
    for i in range(3):
        values.append([i for _ in range(2)])
    return values


print(comp_scope())
