# corpus case — ground truth is CPython at run time (CHARTER I5).
import os
try:
    print(os.keys())
except AttributeError as e:
    print(type(e).__name__)
try:
    print(os.pop("path"))
except AttributeError as e:
    print(type(e).__name__)
