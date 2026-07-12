# Microbenchmark: mixed numeric/string code
# Tests boxed fallback path
s = 0
for i in range(1000):
    s = s + i
print("sum:", s)
