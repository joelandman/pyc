# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: async generator, anext, async for.
import asyncio


async def numbers():
    for i in range(4):
        yield i


async def main():
    out = []
    async for n in numbers():
        out.append(n * 2)
    return out


print(asyncio.run(main()))
