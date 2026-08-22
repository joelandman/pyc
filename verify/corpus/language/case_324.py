# corpus case — ground truth is CPython at run time (CHARTER I5).
class Animal:
    def __init__(self, name):
        self.name = name
class Dog(Animal):
    pass
print(type(Dog('Fido')).__name__)
