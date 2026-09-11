items = [(lambda: i) for i in range(5)]
print([x() for x in items])
