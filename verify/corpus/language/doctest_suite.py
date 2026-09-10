import doctest
def f():
    return type(doctest.DocTestSuite()).__name__
print(f())
