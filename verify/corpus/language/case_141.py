# corpus case — ground truth is CPython at run time (CHARTER I5).
count=0
def inc():
    global count
    count=count+1
inc()
inc()
inc()
print(count)
