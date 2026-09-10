class Meta(type):
    def __new__(cls, name, bases, namespace):
        namespace.pop('__classcell__', None)
        return super().__new__(cls, name, bases, namespace)
try:
    class C(metaclass=Meta):
        def f(self):
            return __class__
    print('no raise')
except RuntimeError as e:
    print('RuntimeError' in type(e).__name__, 'classcell' in str(e))
