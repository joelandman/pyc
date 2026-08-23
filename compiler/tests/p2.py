def sideeffect(n):
    print("evaluated", n)
    return n
r = sideeffect(0) and sideeffect(1)
print(r)
s = sideeffect(5) or sideeffect(6)
print(s)
