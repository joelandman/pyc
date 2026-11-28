# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: correct single except* clause (no return inside the clause).
def run():
    out = []
    try:
        raise ExceptionGroup("g", [ValueError(1), TypeError(2), KeyError(3)])
    except* ValueError as eg:
        out.append(("ve", len(eg.exceptions)))
    return out


print(run())


def run2():
    out = []
    try:
        raise ExceptionGroup("g", [KeyError(2)])
    except* KeyError as eg:
        out.append(("only-key", len(eg.exceptions)))
    return out


print(run2())
