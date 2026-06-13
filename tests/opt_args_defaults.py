import sys

base = [10, 20, 30]

def pick(i, values=base):
    return values[i]

idx = int(sys.argv[1])
print(sys.argv[0] != "")
print(pick(idx))
