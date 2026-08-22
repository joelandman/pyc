# corpus case — ground truth is CPython at run time (CHARTER I5).
try:
    raise KeyError('a\nb')
except KeyError as e:
    print(str(e))
from pathlib import Path
print([Path('a\nb')])
