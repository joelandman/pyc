for i in [1,2,3]:
    print("a", i)
else:
    print("a-else")
for i in [1,2,3]:
    if i == 2: break
    print("b", i)
else:
    print("b-else NOT printed")
n = 0
while n < 3:
    print("c", n); n += 1
else:
    print("c-else")
n = 0
while n < 3:
    if n == 1: break
    print("d", n); n += 1
else:
    print("d-else NOT printed")
for i in []:
    print("never")
else:
    print("empty-else")
def f():
    for i in [1,2,3]:
        if i == 2: return "ret"
    else:
        print("f-else NOT printed")
print(f())
for i in [1,2]:
    for j in [1,2]:
        if j == 2: break
        print("nest", i, j)
    else:
        print("inner-else")
