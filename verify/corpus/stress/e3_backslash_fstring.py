# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: f-string with backslash (valid in 3.12+, may be a parser edge).
name = "world"
greeting = f"hello {name}\nsecond line"
print(greeting)
expr = f"{1 + 2}\ttabled"
print(repr(expr))
