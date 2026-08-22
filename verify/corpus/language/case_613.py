# corpus case — ground truth is CPython at run time (CHARTER I5).
import re
m = re.search("a+", "xxaaa")
print(m.group(0))
print(re.findall("x+", "xx a xxx"))
p = re.compile("[0-9]+")
print(re.search("12", "ab12cd") is not None)
for i in range(8):
    re.findall("a", "aaa")
print("ok")
