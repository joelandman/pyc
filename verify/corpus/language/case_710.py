# corpus case — ground truth is CPython at run time (CHARTER I5).
a=[1.0,2.0,3.0]
i=0
while i<3:
    a[0]=a[0]+a[1]
    i=i+1
print(a[0])
