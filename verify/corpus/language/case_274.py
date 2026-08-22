# corpus case — ground truth is CPython at run time (CHARTER I5).
def spaceship(a, b): return (a > b) - (a < b)
words = ['banana', 'apple', 'cherry']
print(sorted(words, key=cmp_to_key(spaceship)))
