# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# type.__new__ wraps three names when it finds a plain function under them:
# __new__ as a staticmethod, __init_subclass__ and __class_getitem__ as
# classmethods. It tests PyFunction_Check, and a pyc-compiled callable is not a
# PyFunctionObject, so the wrapping was silently skipped for every compiled
# class. __init_subclass__ then never received the subclass.
class Base:
    def __init_subclass__(cls, tag=None, **kw):
        cls.tag = tag

    def __class_getitem__(cls, item):
        return f"{cls.__name__}[{item}]"


class Kid(Base, tag="k"):
    pass


class Plain(Base):
    pass


class Made:
    def __new__(cls, *a):
        return super().__new__(cls)


print("init_subclass with keyword:", Kid.tag)
print("init_subclass without:", Plain.tag)
print("class_getitem:", Base[int])
print("kinds:",
      type(Base.__dict__["__init_subclass__"]).__name__,
      type(Base.__dict__["__class_getitem__"]).__name__,
      type(Made.__dict__["__new__"]).__name__)
print("instantiates:", type(Made()).__name__)
