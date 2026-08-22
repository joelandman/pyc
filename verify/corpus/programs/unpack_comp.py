# Test multi-variable unpacking in list comprehension
# Expected: [(1, 'a'), (2, 'b'), (3, 'c')]

result = [(x, y) for x, y in [(1, 'a'), (2, 'b'), (3, 'c')]]
print(result)
