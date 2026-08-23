try:
    raise ValueError("boom")
except TypeError:
    print("wrong")
except ValueError as e:
    print("right:", e)
