# corpus case — ground truth is CPython at run time (CHARTER I5).
import os
try:
    print(os.get("path"))
except Exception as e:
    print(type(e).__name__)
