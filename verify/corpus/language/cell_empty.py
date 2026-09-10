def outer():
    a = 1
    def inner():
        return a
    del inner.__closure__[0].cell_contents
    try:
        inner()
    except NameError as e:
        print("inner", type(e).__name__)
    try:
        return a
    except UnboundLocalError as e:
        print("outer", type(e).__name__)
    return None

outer()
