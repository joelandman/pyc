# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: ABC with @abstractmethod, subclass implementing it.
from abc import ABC, abstractmethod


class Animal(ABC):
    @abstractmethod
    def speak(self):
        ...

    def describe(self):
        return f"{self.name} says {self.speak()}"


class Dog(Animal):
    def __init__(self):
        self.name = "Rex"

    def speak(self):
        return "woof"


d = Dog()
print(d.describe())
print(d.name)
try:
    Animal()
    print("INSTANTIATED")
except Exception as e:
    print(type(e).__name__)
