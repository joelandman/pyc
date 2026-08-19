# W12.3 / I-228: bound file methods
p = "/tmp/pyc_w123_bound.txt"
f = open(p, "w")
h = f.write
h("hello\n")
h("world")
f.close()
f = open(p)
r = f.read
print(r())
f.close()
f = open(p)
rl = f.readline
print(repr(rl()))
print(repr(rl()))
f.close()
f = open(p)
ls = f.readlines
print(ls())
f.close()
f = open(p)
c = f.close
c()
try:
    f.read()
except ValueError:
    print("closed")
