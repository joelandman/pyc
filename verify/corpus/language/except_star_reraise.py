try:
    try:
        raise ExceptionGroup("eg", [TypeError(1), ValueError(2), OSError(3)])
    except* TypeError as e:
        raise
    except* ValueError as e:
        raise
except ExceptionGroup as e:
    print(sorted(type(x).__name__ for x in e.exceptions))
