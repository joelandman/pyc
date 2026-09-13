from contextlib import ExitStack
def raise_exc(exc):
    raise exc
try:
    with ExitStack() as stack:
        stack.callback(raise_exc, IndexError)
        stack.callback(raise_exc, KeyError)
        1 / 0
except IndexError as e:
    print(type(e.__context__).__name__)
