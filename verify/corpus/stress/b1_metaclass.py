# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: a metaclass injecting members and validating subclass construction.
class Meta(type):
    registry = []

    def __init__(cls, name, bases, ns):
        super().__init__(name, bases, ns)
        Meta.registry.append(name)

    def __call__(cls, *args, **kwargs):
        inst = super().__call__(*args, **kwargs)
        inst.meta_seen = True
        return inst


class WithMeta(metaclass=Meta):
    def __init__(self):
        self.value = 1


print(Meta.registry)
w = WithMeta()
print(w.value)
print(w.meta_seen)
