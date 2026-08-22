# corpus case — ground truth is CPython at run time (CHARTER I5).
def check(x):
    if x:
        return "truthy"
    else:
        return "falsy"

print(check("hello"))
print(check(""))
print(check([1, 2, 3]))
print(check([]))
print(check({"a": 1}))
print(check({}))
print(check([1, "a", 2]))

s = "loop"
count = 0
while s:
    count += 1
    if count >= 3:
        s = ""
print(count)

lst = [1, 2, 3]
x = "yes" if lst else "no"
print(x)
y = "yes" if [] else "no"
print(y)
