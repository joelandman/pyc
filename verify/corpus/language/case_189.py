# corpus case — ground truth is CPython at run time (CHARTER I5).
x=0
for i in range(3):
    if i==2:
        x='done'
    else:
        x=i
print(x)
