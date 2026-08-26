c = compile('print(7)', '<s>', 'exec')
exec(c, {'print': print})
print(eval(compile('3*4', '<s>', 'eval'), {}))
ns = {}
exec('q = 2**70', ns)
print(repr(ns['q']))
