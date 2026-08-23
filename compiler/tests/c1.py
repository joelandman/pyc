print(1 < 2 < 3)
print(3 < 2 < 1)
print(1 < 5 > 2)
print(1 == 1 == 1)
def se(n):
    print("eval", n)
    return n
print(se(9) < se(1) < se(99))
