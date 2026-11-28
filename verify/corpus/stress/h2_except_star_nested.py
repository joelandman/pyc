# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: nested ExceptionGroups and except* matching sub-subgroups. This is
# the trickiest exception semantics in 3.11+; a silent-wrong here is severe.
def flatten(group, depth=0):
    if depth > 4:
        return []
    out = []
    for exc in group.exceptions:
        if isinstance(exc, ExceptionGroup):
            out.extend(flatten(exc, depth + 1))
        else:
            out.append(type(exc).__name__)
    return out


try:
    try:
        raise ExceptionGroup(
            "outer",
            [
                ValueError(1),
                ExceptionGroup("inner", [TypeError(2), KeyError(3)]),
            ],
        )
    except* ValueError as eg:
        print("caught-ve", len(eg.exceptions))
except ExceptionGroup as eg:
    # the ExceptionGroup that survived is the one with TypeError + KeyError
    print("leftover", flatten(eg))


# re-raise inside except* preserving structure
def re():
    try:
        raise ExceptionGroup("g", [ValueError(1), ValueError(2), KeyError(3)])
    except* ValueError as eg:
        raise ExceptionGroup("agg", eg.exceptions) from eg
    except* KeyError:
        pass


try:
    re()
except ExceptionGroup as eg:
    print("agg", flatten(eg))
