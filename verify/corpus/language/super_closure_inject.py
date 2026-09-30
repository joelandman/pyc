# code.replace can add the __class__ cell after the function was compiled.
# CPython's zero-argument super() finds that cell on the frame that runs.
import opcode
from types import FunctionType

def external_getitem(self, i):
    return f"Foreign getitem: {super().__getitem__(i)}"

COPY = opcode.opmap["COPY_FREE_VARS"]

def create_closure(__class__):
    return (lambda: __class__).__closure__

c = external_getitem.__code__
nc = c.replace(co_freevars=c.co_freevars + ("__class__",),
               co_code=bytes([COPY, 1]) + c.co_code)

class List(list):
    pass

List.__getitem__ = FunctionType(
    nc, globals(), "__getitem__", None, create_closure(List))
print(List([1, 2, 3])[0])

def plain(self, i):
    return super().__getitem__(i)

class List2(list):
    pass

List2.__getitem__ = plain
try:
    print(List2([1])[0])
except RuntimeError as e:
    print(type(e).__name__, str(e))

def noargs():
    return super()

try:
    noargs()
except RuntimeError as e:
    print(str(e))
