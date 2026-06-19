# Test recursion
def factorial(n):
    if n <= 1:
        return 1
    return n * factorial(n - 1)

def fibonacci(n):
    if n <= 0:
        return 0
    if n == 1:
        return 1
    return fibonacci(n - 1) + fibonacci(n - 2)

def main():
    print(factorial(5))
    print(factorial(1))
    print(fibonacci(10))

if __name__ == "__main__":
    main()
