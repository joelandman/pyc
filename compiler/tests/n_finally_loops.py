for i in range(5):
    try:
        if i == 2: break
        print("i", i)
    finally:
        print("fin", i)
print("---")
for i in range(4):
    try:
        if i % 2: continue
        print("even", i)
    finally:
        print("f", i)
print("---")
def f():
    for i in range(5):
        try:
            if i == 1: return "ret"
        finally:
            print("ff", i)
print(f())
print("---")
for i in range(3):
    while True:
        break
    print("plain", i)
