# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: __init_subclass__ and __set_name__ class-creation hooks.
class Register:
    created = []

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        Register.created.append(cls.__name__)


class A(Register):
    pass


class B(Register):
    pass


class Slot:
    def __set_name__(self, owner, name):
        self.owner = owner.__name__
        self.attr = name


slot = Slot()


class Holder:
    slot = slot


print(sorted(Register.created))
print(slot.owner, slot.attr)
