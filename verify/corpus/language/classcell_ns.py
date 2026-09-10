ns = {}
class Meta(type):
    def __new__(cls, name, bases, namespace):
        ns['has'] = '__classcell__' in namespace
        return super().__new__(cls, name, bases, namespace)
class A(metaclass=Meta):
    def f(self):
        return __class__
print(ns['has'])
print(A().f() is A)
