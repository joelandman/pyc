# corpus case — ground truth is CPython at run time (CHARTER I5).
def mark(cls):
    cls.marked = True
    return cls

@mark
class Simple:
    pass

print(Simple.marked)
def add_repr(cls):
    def repr_method(self):
        return "Point"
    cls.__repr__ = repr_method
    return cls

@add_repr
class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

p = Point(1, 2)
print(repr(p))
def uppercase_name(cls):
    cls.NAME = 'UPPER'
    return cls
def add_version(cls):
    cls.VERSION = '1.0'
    return cls

@uppercase_name
@add_version
class App:
    pass

print(App.NAME)
print(App.VERSION)
def with_attr(name, value):
    def decorator(cls):
        setattr(cls, name, value)
        return cls
    return decorator

@with_attr('SPECIAL', 'yes')
class Feature:
    pass

print(Feature.SPECIAL)
