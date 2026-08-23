try:
    try:
        raise ValueError("inner")
    except ValueError:
        print("handling")
        raise
except ValueError as e:
    print("re-raised:", e)
