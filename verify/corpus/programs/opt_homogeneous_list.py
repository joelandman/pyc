# Microbenchmark: homogeneous list operations
# Tests list_int/native list subscript operations
lst = [i for i in range(10000)]
s = 0
for x in lst:
    s = s + x
print(s)
