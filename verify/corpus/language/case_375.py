# corpus case — ground truth is CPython at run time (CHARTER I5).
class Base:
    def greet(self):
        return 'base'
class Mid(Base):
    def greet(self):
        return 'mid-' + super().greet()
class Leaf(Mid):
    def greet(self):
        return 'leaf-' + super().greet()
print(Leaf().greet())
