# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: json.dumps default/indent/ensure_ascii and round-trip.
import json


class P:
    def __init__(self, x):
        self.x = x


def default(o):
    if isinstance(o, P):
        return {"__P__": o.x}
    raise TypeError


print(json.dumps({"a": 1, "b": [1, 2, 3]}, indent=2))
print(json.dumps({"k": "café"}, ensure_ascii=False))
print(json.dumps(P(5), default=default))
print(json.loads(json.dumps({"a": 1})))
