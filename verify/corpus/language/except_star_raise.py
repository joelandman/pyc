orig = ExceptionGroup("eg", [ValueError(1), OSError(2)])
try:
    try:
        raise orig
    except* OSError as e:
        raise TypeError(3)
except ExceptionGroup as e:
    print(sorted(type(x).__name__ for x in e.exceptions))
