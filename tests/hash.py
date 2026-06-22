#!/usr/bin/env python3

# --- basic creation and access ---
colors = {"red": 1, "green": 2, "blue": 3}
print(colors["red"])       # 1
print(colors["green"])     # 2
print(colors["blue"])      # 3

# --- insertion and update ---
colors["yellow"] = 4
print(colors["yellow"])    # 4
colors["red"] = 10
print(colors["red"])       # 10

# --- exists ---
if "green" in colors:
    print("green exists")
if "purple" not in colors:
    print("purple missing")

# --- delete ---
del colors["green"]
if "green" not in colors:
    print("green deleted")

# --- sorted keys iteration ---
for k in sorted(colors):
    print(k)

# --- count keys ---
n = len(colors)
print(n)


# --- hash as sub argument ---
def sum_hash(h):
    total = 0
    for k in sorted(h):
        total += h[k]
    return total


nums = {"a": 3, "b": 7, "c": 2}
print(sum_hash(nums))      # 12
