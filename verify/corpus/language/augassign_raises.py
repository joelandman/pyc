# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# An augmented assignment on a BARE NAME whose operator raises used to drop one
# reference too many: lower_augassign marked the loaded value owned a second
# time, mark_owned is an unconditional push_back, and the landing pad therefore
# emitted two decrefs. The object was freed while the binding still pointed at
# it, so the next use was a dangling pointer -- a segfault, not a wrong answer.
#
# Attribute and subscript targets take the call_capi path and were never
# affected. Keeping all three here is what localises a regression.
class Boom:
    def __iadd__(self, other):
        raise ValueError("iadd")

    def __ior__(self, other):
        raise ValueError("ior")


class Box:
    def __init__(self):
        self.f = Boom()


def bare_name():
    m = Boom()
    try:
        m += 1
    except ValueError:
        pass
    return type(m).__name__


def bare_name_or():
    m = Boom()
    try:
        m |= {"a": 1}
    except ValueError:
        pass
    return type(m).__name__


def attribute():
    b = Box()
    try:
        b.f += 1
    except ValueError:
        pass
    return type(b.f).__name__


def subscript():
    d = {"k": Boom()}
    try:
        d["k"] += 1
    except ValueError:
        pass
    return type(d["k"]).__name__


def repeated():
    m = Boom()
    for _ in range(50):
        try:
            m += 1
        except ValueError:
            pass
    return type(m).__name__


print(bare_name())
print(bare_name_or())
print(attribute())
print(subscript())
print(repeated())
