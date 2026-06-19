# List operations benchmark
# Measures list creation, append, and access performance

def main():
    lst = []
    i = 0
    while i < 100000:
        lst.append(i)
        i = i + 1
    
    total = 0
    j = 0
    while j < len(lst):
        total = total + lst[j]
        j = j + 1
    print(total)
