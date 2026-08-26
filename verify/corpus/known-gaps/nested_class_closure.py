# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# A method of a class defined INSIDE a function cannot see that function's
# locals. The closure chain breaks across the class-body scope.
def f():
    output = []

    class T:
        def go(self):
            output.append("called")

    T().go()
    return output


print(f())
