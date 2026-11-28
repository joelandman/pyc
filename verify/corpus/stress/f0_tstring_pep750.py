# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: PEP 750 t-strings (type strings). These are the genuinely-new 3.14
# construct; a silent-wrong or a spurious refusal here is the highest-value
# finding. Verify the t-string *type*, its str, and nested quotes.
t = t"hello {1 + 1}"
print(type(t).__name__)
print(t)
print(repr(t))
# a t-string with an escaped backslash inside, and a non-ASCII name field
name = "café"
t2 = t"unicode:{name}"
print(t2)
# bare t-string with no interpolation
print(t"plain")
