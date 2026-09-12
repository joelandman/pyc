try:
    assert False, (3,)
except AssertionError as e:
    print(str(e))
    print(e.args)
