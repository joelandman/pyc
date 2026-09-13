import pickle
def f():
    global C
    class C:
        pass
    return C()
o = f()
print(type(o).__module__, type(o).__name__, type(o).__qualname__)
g = pickle.loads(pickle.dumps(o))
print(type(g).__name__, g.__dict__)
