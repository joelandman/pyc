#!/usr/bin/env python3
import re

# --- while with /g, no captures ---
s = "cat bat hat"
for _m in re.finditer(r"\w+at", s):
    print("match")   # match x3

# --- while with /g and captures ---
data = "x=1 y=2 z=3"
for m in re.finditer(r"(\w+)=(\d+)", data):
    print(m.group(1))   # x, y, z
    print(m.group(2))   # 1, 2, 3

# --- loop reuse: same var, fresh loop ---
s2 = "aa bb cc"
for m in re.finditer(r"(\w+)", s2):
    print(m.group(1))   # aa, bb, cc

# --- for/foreach with /g (list context) ---
words = re.findall(r"(\w+)", "hello world foo")
print(words[0])         # hello
print(words[1])         # world
print(words[2])         # foo
print(len(words))       # 3

# --- foreach loop variable ---
for m in re.findall(r"([a-z]+)", "one1 two2 three3"):
    print(m)            # one, two, three
