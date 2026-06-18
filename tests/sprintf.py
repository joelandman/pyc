#!/usr/bin/env python3

# basic string and integer
print("Hello, %s! You are %d years old." % ("Alice", 30))

# floats
print("Pi is approximately %.4f" % 3.14159265)
print("%e" % 12345.6789)
print("%g" % 0.000123)

# width and alignment
print("[%10s]" % "right")
print("[%-10s]" % "left")
print("[%05d]" % 42)

# sprintf returns a string
s = "(%s, %d)" % ("foo", 99)
print(s)

# hex and octal
print("%x" % 255)
print("%X" % 255)
print("%o" % 8)

# multiple uses
for i in range(1, 4):
    line = "item %02d: %s" % (i, "value")
    print(line)

# width from argument
print("%*d" % (8, 42))

# precision from argument
print("%.*f" % (3, 3.14159))

# %% literal
print("100%")

# sprintf used in concatenation
prefix = "Result: "
result = prefix + "%d + %d = %d" % (3, 4, 7)
print(result)
