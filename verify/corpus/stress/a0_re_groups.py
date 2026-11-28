# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: re groups, search vs fullmatch, alternation, lookahead, sub.
import re


m = re.search(r"(\w+)@(\w+)", "mail user@example.com end")
print(m.group(0))
print(m.group(1), m.group(2))

print(re.findall(r"\d+", "a1 b22 c333"))

print(re.sub(r"\d", "#", "a1b2"))

print(bool(re.fullmatch(r"(abc)+", "abcabc")))

print(re.split(r"[,;]", "a,b;c;;d"))
