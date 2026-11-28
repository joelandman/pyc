# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: async context manager (__aenter__/__aexit__) and async with.
import asyncio


class AsyncRes:
    def __init__(self):
        self.log = []

    async def __aenter__(self):
        self.log.append("enter")
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.log.append("exit")
        return False


async def main():
    r = AsyncRes()
    async with r as res:
        res.log.append("body")
    return r.log


print(asyncio.run(main()))
