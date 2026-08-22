#!/usr/bin/env python3
import re

# --- basic match ---
s = "Hello, World!"
if re.search(r"World", s):
    print("match")          # match
if not re.search(r"xyz", s):
    print("no xyz")         # no xyz

# --- case-insensitive ---
if re.search(r"hello", s, re.IGNORECASE):
    print("icase")          # icase

# --- capture groups ---
date = "2024-03-15"
m = re.search(r"(\d{4})-(\d{2})-(\d{2})", date)
if m:
    print(m.group(1))       # 2024
    print(m.group(2))       # 03
    print(m.group(3))       # 15

# --- substitution ---
string = "foo bar foo"
string = re.sub(r"foo", "baz", string, count=1)
print(string)               # baz bar foo

# --- global substitution ---
str2 = "aabbcc"
str2 = re.sub(r"b", "x", str2)
print(str2)                 # aaxxcc

# --- substitution with capture ---
str3 = "hello world"
str3 = re.sub(r"(\w+)", r"[\1]", str3)
print(str3)                 # [hello] [world]

# --- split with regex ---
csv = "one,,two,,three"
parts = re.split(r",+", csv)
print(parts[0])             # one
print(parts[1])             # two
print(parts[2])             # three

# --- match negation ---
num = "12345"
if not re.search(r"[a-z]", num):
    print("digits only")    # digits only
