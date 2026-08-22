# corpus case — ground truth is CPython at run time (CHARTER I5).
class Animal:
    def speak(self):
        return 'generic sound'
class Dog(Animal):
    def speak(self):
        return 'bark'
d = Dog()
print(Animal.speak(d))
