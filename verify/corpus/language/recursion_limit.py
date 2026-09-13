import sys
from test import support

def recursive_function(depth):
    if depth:
        recursive_function(depth - 1)

with support.infinite_recursion(5):
    available = support.get_recursion_available()
    recursive_function(available)
    try:
        recursive_function(available + 1)
        print("no RecursionError")
    except RecursionError:
        print("RecursionError")
