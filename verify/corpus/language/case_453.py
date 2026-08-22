# corpus case — ground truth is CPython at run time (CHARTER I5).
    def make_const():
        return lambda x: x + 100
    print(make_const()(20))
    
