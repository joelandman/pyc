// example.py - Sample Python 3 program for testing the compiler
# This demonstrates various Python features supported by pyc

import math

def factorial(n):
    """Calculate factorial of n"""
    if n <= 1:
        return 1
    return n * factorial(n - 1)

def fibonacci(n):
    """Generate fibonacci sequence up to n terms"""
    seq = []
    a, b = 0, 1
    for _ in range(n):
        seq.append(a)
        a, b = b, a + b
    return seq

class Shape:
    """Base shape class"""
    def __init__(self):
        self.name = "Shape"
    
    def area(self):
        return 0
    def perimeter(self):
        return 0

class Rectangle(Shape):
    """Rectangle class inheriting from Shape"""
    def __init__(self, width, height):
        super().__init__()
        self.name = "Rectangle"
        self._width = width
        self._height = height
    
    def area(self):
        return self._width * self._height
    
    def perimeter(self):
        return 2 * (self._width + self._height)

class Circle(Shape):
    """Circle class inheriting from Shape"""
    def __init__(self, radius):
        super().__init__()
        self.name = "Circle"
        self._radius = radius
    
    def area(self):
        return math.pi * self._radius ** 2
    
    def perimeter(self):
        return 2 * math.pi * self._radius

def main():
    # Test factorial
    x = 5
    result = factorial(x)
    print(f"Factorial of {x} is {result}")
    
    # Test fibonacci
    fib = fibonacci(10)
    print(f"Fibonacci sequence: {fib}")
    
    # Test class inheritance
    shapes = [
        Rectangle(4, 5),
        Circle(7)
    ]
    
    total_area = 0
    for shape in shapes:
        area = shape.area()
        perimeter = shape.perimeter()
        print(f"{shape.name}: area={area:.2f}, perimeter={perimeter:.2f}")
        total_area += area
    
    print(f"Total area: {total_area:.2f}")
    
    # Test list comprehension
    squares = [i**2 for i in range(10) if i % 2 == 0]
    print(f"Squares of even numbers: {squares}")
    
    # Test exception handling
    try:
        value = 1 / 0
    except ZeroDivisionError as e:
        print("Caught division by zero!")
    except Exception as e:
        print(f"Caught exception: {e}")
    finally:
        print("Cleanup complete")
    
    # Test ternary operator
    status = "positive" if x > 0 else "negative"
    print(f"{x} is {status}")
    
    # Test tuple unpacking
    x, y = (10, 20)
    print(f"Unpacked: x={x}, y={y}")
    
    # Test dict operations
    colors = {
        "red": 255,
        "green": 128,
        "blue": 64
    }
    for name, value in colors.items():
        print(f"{name}: {value}")
    
    print("All tests passed!")

if __name__ == "__main__":
    main()
