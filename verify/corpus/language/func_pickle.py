import pickle
def f(x):
    return x + 1
g = pickle.loads(pickle.dumps(f))
print(g is f, g(3))
