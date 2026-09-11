try:
    raise IndexError
except IndexError as e:
    print(type(e.__traceback__).__name__, len(dir(e.__traceback__)))
