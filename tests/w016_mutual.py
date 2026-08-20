def even(n):
    if n == 0:
        return True
    return odd(n - 1)

def odd(n):
    if n == 0:
        return False
    return even(n - 1)

def a(n):
    if n <= 0:
        return 1
    return b(n - 1) + 1

def b(n):
    if n <= 0:
        return 2
    return a(n - 1) + 1

print(even(4))
print(odd(5))
print(even(1))
print(a(3))
