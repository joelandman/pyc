type RecursiveAlias = dict[str, RecursiveAlias]
print(RecursiveAlias.__value__)
type Boom = 1 / 0
print("defined")
try:
    Boom.__value__
    print("eager")
except ZeroDivisionError:
    print("lazy")
