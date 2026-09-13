from typing import NotRequired, ReadOnly, Required, TypedDict
import annotationlib

class Y(TypedDict):
    a: None
    b: "int"

print(type(Y).__name__)
print("__annotate__" in Y.__dict__)
print(Y.__annotate__.__qualname__)
print(Y.__annotations__["a"] is type(None))
print(type(Y.__annotations__["b"]).__name__)
fwd = Y.__annotate__(annotationlib.Format.FORWARDREF)
print(fwd["a"] is type(None), type(fwd["b"]).__name__)

class A(TypedDict):
    x: NotRequired[undefined]
    y: ReadOnly[undefined]
    z: Required[undefined]

print(sorted(A.__required_keys__))
print(sorted(A.__optional_keys__))
print(sorted(A.__readonly_keys__))
try:
    A.__annotations__
    print("value-ok")
except NameError:
    print("value-nameerror")
s = A.__annotate__(annotationlib.Format.STRING)
print(s["x"], s["y"], s["z"])

def nested():
    class Z(TypedDict):
        a: undefined
    try:
        Z.__annotations__
        print("nested-value-ok")
    except NameError:
        print("nested-nameerror")
    undefined = None
    print(Z.__annotations__["a"] is type(None))

nested()
