# Microbenchmark: function calls with numeric args
# Tests specialized variant dispatch
def add(a, b):
    return a + b

s = 0
for i in range(10000):
    s = add(s, i)
print(s)
