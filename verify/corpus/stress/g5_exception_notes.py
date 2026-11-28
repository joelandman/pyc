# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: exception chaining details — __suppress_context__, __notes__,
# sys.last_* attributes, exception __cause__ vs __context__ precedence, and
# raising while another is being handled (implicit __context__).
def ctx_then_from():
    try:
        try:
            raise ValueError("context")
        except ValueError:
            raise RuntimeError("wrapped")
    except RuntimeError as e:
        # __cause__ is None here (no `from`), __context__ is the ValueError
        return (e.__cause__ is None, isinstance(e.__context__, ValueError))


def notes_test():
    try:
        raise KeyError("k").add_note("note one")
    except KeyError as e:
        return list(e.__notes__)


def notes_multi():
    try:
        raise RuntimeError("boom", )
    except RuntimeError as e:
        e.add_note("first")
        e.add_note("second")
        return list(e.__notes__)


print(ctx_then_from())
print(notes_test())
print(notes_multi())
