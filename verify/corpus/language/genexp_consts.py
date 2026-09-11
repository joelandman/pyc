def f_all():
    return all(x-2 for x in [1,2,3])

code_objs = [c for c in f_all.__code__.co_consts if type(c) is type(f_all.__code__)]
print(len(code_objs), f_all())
