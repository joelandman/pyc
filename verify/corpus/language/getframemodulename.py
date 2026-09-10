import sys
def f():
    return sys._getframemodulename(0)
print(f())
