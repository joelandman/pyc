try:
    try:
        raise OSError("blah")
    except* ExceptionGroup:
        print("caught")
except TypeError:
    print("ok")
