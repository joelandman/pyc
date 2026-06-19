# String operations benchmark
# Measures string creation and concatenation performance

def main():
    result = ""
    i = 0
    while i < 10000:
        result = result + str(i)
        i = i + 1
    print(len(result))
