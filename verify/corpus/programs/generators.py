# Basic generator with yield
def gen():
    yield 1
    yield 2
    yield 3

result = []
for x in gen():
    result.append(x)
print(result)

# Generator with yield from
def inner():
    yield 1
    yield 2
    yield 3

def outer():
    yield from inner()
    yield 4

print(list(outer()))

# Generator with return value
def gen_with_return():
    yield 1
    yield 2
    return 42

print(list(gen_with_return()))

# Nested generators
def nested_gen():
    def inner_gen():
        yield 'a'
        yield 'b'
    for x in inner_gen():
        yield x

print(list(nested_gen()))

# Generator expression
print(list(x*2 for x in range(5)))
