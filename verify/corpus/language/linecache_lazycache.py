import linecache
fn = linecache.__file__ + ".missing"
linecache.clearcache()
linecache.getlines(fn, globals())
print(len(linecache.cache.get(fn, ())))
print(linecache.lazycache(fn, globals()))
