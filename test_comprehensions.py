# Test list comprehension
x = [i for i in range(5)]
print(x)

# Test dictionary comprehension  
y = {i: i*i for i in range(5)}
print(y)

# Test *args
def func(a, b, c):
    print(a, b, c)

args = [1, 2, 3]
func(*args)