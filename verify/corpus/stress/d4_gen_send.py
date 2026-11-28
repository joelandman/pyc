# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: generator .send()/.throw()/.close() protocol.
def counter():
    n = 0
    while True:
        try:
            received = yield n
        except GeneratorExit:
            print("closed")
            raise
        if received is not None:
            n = received
        else:
            n += 1


g = counter()
print(next(g))
print(g.send(10))
print(g.send(20))
print(g.send(30))
g.close()
print("done")


def raising():
    try:
        yield 1
    except ValueError as e:
        print(f"caught:{e}")
        yield 2


g2 = raising()
print(next(g2))
g2.throw(ValueError("boom"))
