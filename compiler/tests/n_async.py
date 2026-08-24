import asyncio, inspect
class Res:
    async def __aenter__(self):
        print("  enter"); return 7
    async def __aexit__(self, *a):
        print("  exit"); return False
def make(base):
    async def scaled(n):
        await asyncio.sleep(0)
        return n * base
    return scaled
async def uses_with():
    async with Res() as v:
        return v + 1
async def counter(k):
    for i in range(k):
        await asyncio.sleep(0)
        yield i
class Svc:
    async def fetch(self, x):
        await asyncio.sleep(0)
        return "got:" + str(x)
async def main():
    print("closure:", await make(10)(4))
    print("async with:", await uses_with())
    print("gather:", await asyncio.gather(make(2)(1), make(3)(2)))
    print("async for:", [i async for i in counter(3)])
    print("method:", await Svc().fetch(9))
    try:
        await failing()
    except ValueError as e:
        print("raised:", e)
async def failing():
    raise ValueError("boom")
asyncio.run(main())
print("iscoroutinefunction:", inspect.iscoroutinefunction(uses_with))
print("isasyncgenfunction:", inspect.isasyncgenfunction(counter))
print("qualname:", make(1).__qualname__)
