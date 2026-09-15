def f():
    a = 1
    b = [1, 2, 3, 4]
    genexp = (c := i + a for i in b)
    print("before", "c" in locals())
    print(list(genexp))
    v = locals().get("c")
    print("after", "c" in locals(), type(v).__name__, v)

f()
