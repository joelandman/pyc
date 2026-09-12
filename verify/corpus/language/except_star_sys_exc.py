import sys
eg = ExceptionGroup("test2", [ValueError("V1"), ValueError("V2")])
try:
    raise eg
except* ValueError as e:
    print(e is sys.exception())
