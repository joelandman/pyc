# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: a minimal async program (coroutines, await, gather).
import asyncio


async def fetch(name, delay):
    await asyncio.sleep(delay)
    return f"{name}-{delay}"


async def main():
    results = await asyncio.gather(
        fetch("a", 0.01),
        fetch("b", 0.02),
        fetch("c", 0.01),
    )
    return results


print(sorted(asyncio.run(main())))
