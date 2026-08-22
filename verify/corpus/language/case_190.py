# corpus case — ground truth is CPython at run time (CHARTER I5).
z=42
for k in range(2):
    if k==1:
        z='end'
    else:
        z=k
print(z)
