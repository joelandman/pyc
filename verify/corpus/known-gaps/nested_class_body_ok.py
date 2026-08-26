# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# The control. A class BODY reading the enclosing function local is correct;
# only a method two scopes down fails. That narrows the defect to the method's
# closure, not to class scoping in general.
def f():
    n = 7

    class T:
        v = n

    return T.v


print(f())
