# corpus case — ground truth is CPython at run time (CHARTER I5).
class A:
    def __init__(self, name):
        self.name = name
    def speak(self):
        return "A:" + self.name
class B(A):
    def __init__(self, name):
        super().__init__(name)
    def speak(self):
        return "B(" + super().speak() + ")"
b = B("rex")
print(b.name)
print(b.speak())
