# corpus case — ground truth is CPython at run time (CHARTER I5).
for i in range(3):
    try:
        if i == 1:
            break
        print("i", i)
    finally:
        print("fin", i)
print("done")
for i in range(3):
    try:
        if i == 1:
            continue
        print("j", i)
    finally:
        print("cfin", i)
def h():
    for i in range(5):
        try:
            if i == 2:
                return i * 10
        finally:
            print("hfin", i)
print(h())
try:
    raise ValueError("v")
except ValueError:
    print("integrity ok")
