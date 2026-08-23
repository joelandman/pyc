assert 1 == 1
try:
    assert 1 == 2, "values differ"
except AssertionError as e:
    print("assert:", e)
g = 5
print(g)
del g
try:
    print(g)
except NameError as e:
    print("deleted:", e)
