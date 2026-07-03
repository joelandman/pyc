# Tight numeric loop benchmark
# Measures loop and arithmetic performance

def main():
    total = 0
    i = 0
    while i < 1000000:
        total = total + i
        i = i + 1
    print(total)
