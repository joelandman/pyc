# Microbenchmark: pure numeric loop with range
# Tests native range loop counters and native arithmetic
s = 0
for i in range(100000):
    s = s + i * 2
print(s)
