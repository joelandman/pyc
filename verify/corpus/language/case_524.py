# corpus case — ground truth is CPython at run time (CHARTER I5).
import re

print(re.search("Hello", "hello world") is None)
print(re.search("hello", "hello world") is not None)

m = re.search("hello", "HELLO WORLD", re.IGNORECASE)
print(m.group(0))
m2 = re.search("hello", "HELLO WORLD", flags=re.IGNORECASE)
print(m2.group(0))

print(re.match("hello", "hello world") is not None)
print(re.match("Hello", "hello world") is None)

print(re.findall("a", "AaAaA", re.IGNORECASE))
for mm in re.finditer("a", "AaA", re.IGNORECASE):
    print(mm.group(0))

print(re.sub("cat", "dog", "Cat cat CAT", flags=re.IGNORECASE))
print(re.sub("cat", "dog", "Cat cat CAT", count=1, flags=re.IGNORECASE))

print(re.split(",", "a,b,c,d"))
print(re.split(",", "a,b,c,d", maxsplit=2))
print(re.split("a", "aXaYaZ", flags=re.IGNORECASE))

p = re.compile("hello", re.IGNORECASE)
print(p is not None)

print(re.findall("^b", "a\nb\nc", re.MULTILINE))
print(re.search("a.b", "a\nb", re.DOTALL) is not None)
print(re.search("a.b", "a\nb") is not None)

# Match.group() through untyped function parameter (fixed: was returning None)
def grp_of(m, i):
    return m.group(i)
mg = re.search(r"(\w+) (\w+)", "hello world")
print(grp_of(mg, 1))
print(grp_of(mg, 2))
