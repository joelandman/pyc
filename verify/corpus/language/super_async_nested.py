class B:
    async def f(self):
        return "B"
class O:
    def make(self):
        class C(B):
            async def f(self):
                return await super().f()
        return C()
c = O().make()
coro = c.f()
try:
    coro.send(None)
except StopIteration as e:
    print(e.args[0] if e.args else None)
