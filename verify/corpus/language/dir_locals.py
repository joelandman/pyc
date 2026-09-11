def f():
    local_var = 1
    return 'local_var' in dir()

print(f())
