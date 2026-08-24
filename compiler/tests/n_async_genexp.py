import asyncio
async def src(k):
    for i in range(k):
        yield i
def maker(s):
    return (x * 10 async for x in s)
async def main():
    print("async genexp:", [v async for v in maker(src(3))])
    g = (y + 1 async for y in src(2))
    print("inline:", [v async for v in g])
asyncio.run(main())
