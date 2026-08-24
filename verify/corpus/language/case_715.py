for v in (None, True, False, 0, 1, ""):
    match v:
        case None: print(repr(v), "None")
        case True: print(repr(v), "True")
        case False: print(repr(v), "False")
        case _: print(repr(v), "other")
