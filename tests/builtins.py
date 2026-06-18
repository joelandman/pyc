#!/usr/bin/env python3
import re


def chomp(s):
    # Perl chomp removes a single trailing newline
    return s[:-1] if s.endswith("\n") else s


# --- shift / unshift ---
a = [1, 2, 3, 4, 5]
first = a.pop(0)
print(first)         # 1
print(len(a))        # 4

a[0:0] = [10, 20]    # unshift @a, 10, 20
print(a[0])          # 10
print(a[1])          # 20
print(len(a))        # 6

# --- chomp ---
line = "hello\n"
line = chomp(line)
print(line)          # hello
print(len(line))     # 5

# --- length ---
s = "abcdef"
print(len(s))        # 6
print(len("xy"))     # 2

# --- substr ---
print(s[2:])         # cdef
print(s[1:1 + 3])    # bcd
print(s[-2:])        # ef
print(s[1:1 + 2])    # bc

# --- join ---
words = ["one", "two", "three"]
print(", ".join(words))            # one, two, three
print("-".join(["a", "b", "c"]))   # a-b-c
print("".join(words))              # onetwothree

# --- split ---
csv = "a,b,c,d"
parts = csv.split(",")
print(len(parts))    # 4
print(parts[0])      # a
print(parts[3])      # d

sentence = "  hello   world  foo  "
ws = sentence.split()   # split(" ", ...) in Perl == awk-style whitespace split
print(len(ws))       # 3
print(ws[0])         # hello
print(ws[2])         # foo

# split with regex-style pattern
path = "usr/local/bin"
dirs = re.split(r"/", path)
print(len(dirs))     # 3
print(dirs[1])       # local

# --- round-trip: split then join ---
orig = "one:two:three"
bits = orig.split(":")
rejoined = ":".join(bits)
print(rejoined)      # one:two:three

# --- chomp in a loop ---
lines = ["foo\n", "bar\n", "baz\n"]
for l in lines:
    l = chomp(l)
    print(l)         # foo, bar, baz
