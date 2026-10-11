# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# Class-body `del name` is DELETE_NAME: the class namespace only. A missing
# name is NameError, and a global of the same spelling stays. The annotation
# `x: int` still builds C.__annotations__ after that NameError is caught.

def show(label, fn):
    try:
        print(label, "ok", fn())
    except Exception as e:
        ctx = e.__context__
        print(label, type(e).__name__, str(e), getattr(e, "name", None),
              type(ctx).__name__ if ctx else None)


def missing():
    class C:
        del missing_name


def present():
    class C:
        x = 1
        del x
    return "x" in C.__dict__


g = "global"


def global_stays():
    class C:
        del g
    return g


h = 7


def class_not_global():
    class D:
        h = 8
        del h
    return ("h" in D.__dict__, h)


def mangled():
    class A:
        __x = 1
        del __x
    return [k for k in A.__dict__ if "x" in k]


class NS(dict):
    def __delitem__(self, k):
        raise TypeError("nope")


class Meta(type):
    @classmethod
    def __prepare__(cls, name, bases):
        return NS()


def custom():
    class C(metaclass=Meta):
        z = 1
        del z


def annotations():
    saved = globals().pop("__annotations__", None)
    try:
        class C:
            try:
                del __annotations__
            except NameError:
                pass
            x: int
        return C.__annotations__
    finally:
        if saved is not None:
            globals()["__annotations__"] = saved


show("missing", missing)
show("present", present)
show("global", global_stays)
show("both", class_not_global)
show("mangled", mangled)
show("custom", custom)
show("annotations", annotations)
