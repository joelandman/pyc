# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: __slots__ restricting instance dicts and inheritance.
class Node:
    __slots__ = ("value", "left", "right")

    def __init__(self, value):
        self.value = value
        self.left = None
        self.right = None


n = Node(5)
print(n.value, n.left, n.right)
try:
    n.other = 1
    print("ALLOWED")
except AttributeError:
    print("AttributeError")


class Child(Node):
    __slots__ = ()


c = Child(7)
print(c.value)
try:
    c.extra = 1
    print("ALLOWED")
except AttributeError:
    print("AttributeError")
