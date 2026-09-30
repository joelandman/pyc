import sys

def already(s):
    # Same shape as Lib/test/test_code.isinterned: a newly built equal
    # string collapses onto s only when s was already interned.
    return s is sys.intern(("_" + s + "_")[1:-1])

def f(a="str_value"):
    return a

print(already(f()))
print(already("str_value"))
print(already("hello world"))
print(already("str\0value!"))
print(already(""))
print(already("caf\u00e9"))
