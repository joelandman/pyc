def fbad():
    x: int
    print(x)
try:
    fbad()
except UnboundLocalError:
    print("ok")
