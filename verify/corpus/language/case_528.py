# corpus case — ground truth is CPython at run time (CHARTER I5).
import string
print(string.ascii_lowercase)
print(string.ascii_uppercase)
print(string.digits)
print(string.punctuation)

import textwrap
text = "This is a long piece of text that should wrap across multiple lines when given a narrow width."
print(textwrap.wrap(text, 20))
print(textwrap.fill(text, 20))
print(textwrap.wrap("short text"))

import copy
a = [1, 2, [3, 4]]
b = copy.copy(a)
b[2].append(999)
print(a)
print(b)
print(a[2] is b[2])

c = copy.deepcopy(a)
c[2].append(100)
print(a, c)

from copy import deepcopy
d = {"x": [1, 2]}
e = deepcopy(d)
e["x"].append(3)
print(d, e)

import uuid
u = str(uuid.uuid4())
print(len(u))
print(u.count("-"))
parts = u.split("-")
print([len(p) for p in parts])
print(parts[2][0])
