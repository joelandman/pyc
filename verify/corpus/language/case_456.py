# corpus case — ground truth is CPython at run time (CHARTER I5).
    def make_add_twenty():
        return lambda x: x + 20
    add20 = make_add_twenty()
    print(add20(10))
    
