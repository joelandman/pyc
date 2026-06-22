# examples/simple.py
# Compile with: pyc simple.py -o simple --opt=2 && ./simple

def add(a, b):
    return a + b

x = add(2, 3)
if x > 0:
    print(x)
else:
    print(0)

i = 0
while i < 3:
    print(i)
    i = add(i, 1)

print(42)
