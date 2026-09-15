import sys, types, traceback

def f():
    return sys._getframe()

tb = types.TracebackType(None, f(), 0, f.__code__.co_firstlineno)
exc = KeyboardInterrupt()
exc.__traceback__ = tb
print(repr("".join(traceback.format_exception(type(exc), exc, tb))))
