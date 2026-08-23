print([x for row in [[1,2],[3,4]] for x in row])
print([(a,b) for a in [1,2] for b in "xy"])
print([x for row in [[1,2],[3,4]] for x in row if x % 2 == 0])
print({k: v for pair in [[("a",1)],[("b",2)]] for k, v in pair})
