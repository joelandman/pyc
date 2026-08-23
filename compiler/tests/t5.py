try:
    try:
        raise RuntimeError("inner")
    except ValueError:
        print("wrong handler")
except RuntimeError as e:
    print("propagated to outer:", e)
