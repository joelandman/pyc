class C:
    def meth(self):
        pass
print("__firstlineno__" in C.__dict__)
print(type(C.__dict__["__firstlineno__"]).__name__)
