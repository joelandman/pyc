from annotationlib import Format, get_annotations

def f(x: Undefined):
    pass

a = get_annotations(f, format=Format.FORWARDREF)
print(type(a["x"]).__name__)
