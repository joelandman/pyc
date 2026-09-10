# Star as a value expression is a SyntaxError in CPython; as a call/list
# unpack it must run. I1a: the lower.cpp star-unpacking refusal is for the
# leftover expr-context Starred node.
print(*[1, 2], *[3])
print([*range(2), 9])
print({*range(2)})
