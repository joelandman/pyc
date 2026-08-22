# I-011: type() returns a type object
print(type(1))
print(type(1).__name__)
print(type(1) is type(2))
print(type(1.0).__name__)
print(type(None).__name__)
print(type(True).__name__)
print(isinstance(1, type(1)))
print(isinstance(1.0, type(1)))
print(type(type(1)).__name__)
class Dog:
    pass
print(type(Dog()).__name__)
print(type(Dog()) is Dog)
print(isinstance(Dog(), Dog))

def f(y):
    return y + 0.5
print(f(1.5))
print(f(2.5))
