# Function call overhead benchmark
# Measures function call performance

def add(a, b):
    return a + b

def main():
    total = 0
    i = 0
    while i < 100000:
        total = add(total, i)
        i = i + 1
    print(total)
