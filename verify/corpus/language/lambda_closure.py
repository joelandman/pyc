f1 = lambda x: lambda y: x + y
print(f1(1)(1), f1(10)(5))
f2 = lambda x: (lambda : lambda y: x + y)()
print(f2(1)(1), f2(10)(5))
f8 = lambda x, y, z: lambda a, b, c: lambda : z * (b + y)
print(f8(1, 2, 3)(2, 4, 6)())
