def shout(f):
    def inner(name):
        return f(name).upper()
    return inner
@shout
def greet(n):
    return "hi " + n
print(greet("bob"))
