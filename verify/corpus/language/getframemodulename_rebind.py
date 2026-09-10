import sys
def f():
    return sys._getframemodulename(0)
__name__ = "test.test_metaclass"
print(f())
