# Direct method calls: lookup before arguments, self passed without a
# bound method for a function or a method descriptor. Starred calls and a
# bare attribute load still bind. Oracle is CPython.

log = []

def arg(tag):
    log.append("arg:" + tag)
    return tag

class C:
    def m(self, x):
        log.append("m:" + x)
        return (self.tag, x)

    def k(self, x, y=0):
        return (self.tag, x, y)

    @classmethod
    def cm(cls, x):
        return (cls.__name__, x)

    @staticmethod
    def sm(x):
        return ("static", x)

c = C()
c.tag = "c"

print("pos", c.m(arg("a")))
print("kw", c.k(arg("b"), y=3))
print("kwonly", c.k(y=4, x=arg("c")))
print("zero", c.cm("z")[0], c.sm("s"), C.sm("t"))
print("cls", c.cm(arg("d")))
saved = c.m
print("bound", saved(arg("e")))
print("star", c.m(*["f"]))
print("star2", c.k(*["g"], **{"y": 9}))
print("builtin", "ab".upper(), [1, 2].count(1))

class Box:
    @property
    def obj(self):
        log.append("prop")
        return c

print("prop", Box().obj.m(arg("h")))

class Late:
    def __getattr__(self, name):
        log.append("miss:" + name)
        if name == "late":
            def late(x):
                log.append("late:" + x)
                return x
            return late
        raise AttributeError(name)

late = Late()
print("late", late.late(arg("i")))
try:
    late.missing(arg("j"))
    print("missing", "no-error")
except AttributeError as e:
    print("missing", type(e).__name__)

class Over:
    def m(self, x):
        return ("type", x)
o = Over()
o.m = lambda x: ("inst", x)
print("over", o.m(arg("k")))

print("log", ",".join(log))
