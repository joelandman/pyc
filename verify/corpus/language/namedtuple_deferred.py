from typing import NamedTuple

def test():
    class X(NamedTuple):
        y: undefined

    print("fields", X._fields)
    try:
        print("ann_before", X.__annotations__)
    except Exception as e:
        print("ann_before_exc", type(e).__name__, str(e))

    undefined = int
    try:
        print("ann_after", X.__annotations__)
    except Exception as e:
        print("ann_after_exc", type(e).__name__, str(e))

test()
