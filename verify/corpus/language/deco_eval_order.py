actions = []

def make_decorator(tag):
    actions.append('makedec' + tag)
    def decorate(func):
        actions.append('calldec' + tag)
        return func
    return decorate

class NameLookupTracer(object):
    def __init__(self, index):
        self.index = index
    def __getattr__(self, fname):
        if fname == 'make_decorator':
            opname, res = ('evalname', make_decorator)
        elif fname == 'arg':
            opname, res = ('evalargs', str(self.index))
        else:
            raise AssertionError(fname)
        actions.append('%s%d' % (opname, self.index))
        return res

c1, c2, c3 = map(NameLookupTracer, [1, 2, 3])

@c1.make_decorator(c1.arg)
@c2.make_decorator(c2.arg)
@c3.make_decorator(c3.arg)
def foo():
    return 42

print(foo())
print(actions)
