import _testcapi

class CM:
    def __enter__(self):
        return self
    def __exit__(self, *a):
        return False

cm = CM()
hit = False
_testcapi.set_nomemory(0, 0)
try:
    with cm:
        hit = True
finally:
    _testcapi.remove_mem_hooks()
print("ok" if hit else "fail")
