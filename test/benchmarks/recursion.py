# Recursion depth benchmark
# Measures recursive function performance

def factorial(n):
    if n <= 1:
        return 1
    return n * factorial(n - 1)

def main():
    result = factorial(20)
    print(result)
