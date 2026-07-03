# Dict operations benchmark
# Measures dict creation, insertion, and access performance

def main():
    d = {}
    i = 0
    while i < 50000:
        d[str(i)] = i * 2
        i = i + 1
    
    total = 0
    j = 0
    while j < 50000:
        key = str(j)
        if key in d:
            total = total + d[key]
        j = j + 1
    print(total)
