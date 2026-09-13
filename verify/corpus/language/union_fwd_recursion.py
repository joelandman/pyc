from typing import List, Union, get_type_hints

def test():
    ValueList = List["Value"]
    Value = Union[str, ValueList]

    class C:
        foo: List[Value]

    class D:
        foo: Union[Value, ValueList]

    class E:
        foo: Union[List[Value], ValueList]

    class F:
        foo: Union[Value, List[Value], ValueList]

    g, l = globals(), locals()
    for name, cls in ("C", C), ("D", D), ("E", E), ("F", F):
        h = get_type_hints(cls, g, l)
        print(name, h)

test()
