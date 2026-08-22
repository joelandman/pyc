# corpus case — ground truth is CPython at run time (CHARTER I5).
p = "/tmp/pyc_test_uaf_regression.txt"
with open(p, "w") as f:
    f.write("hello")
with open(p, "r") as f:
    pass
with open(p, "r") as f:
    pass
with open(p, "r") as f:
    lines = f.readlines()
print(lines)
