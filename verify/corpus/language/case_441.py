# corpus case — ground truth is CPython at run time (CHARTER I5).
for v in [1, 2, 3, 4, 5]:
    if v == 3:
        continue
    if v == 5:
        break
    print(v)
