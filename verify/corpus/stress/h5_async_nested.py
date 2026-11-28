# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: async for / async await in nested coroutines and async generator
# with throw/close; also mixing await in a comprehension is invalid, so this is
# the legal async surface.
import asyncio


async def fetch(vals):
    return [v * 2 async for v in (async_range(len(vals)))]


async def async_range(n):
    for i in range(n):
        yield i


async def gather():
    tasks = [asyncio.create_task(fetch([1, 2, 3])) for _ in range(3)]
    return await asyncio.gather(*tasks)


print(sorted(asyncio.run(gather())))


async def agen():
    for i in range(3):
        yield i


async def drain():
    out = []
    async for x in agen():
        out.append(x + 100)
    return out


print(asyncio.run(drain()))
