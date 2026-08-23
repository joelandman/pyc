def shout(f):
    def inner(name):
        return f(name).upper()
    return inner
@shout
def greet(name):
    return "hi " + name
print(greet("bob"))
