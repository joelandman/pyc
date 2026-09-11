class AsyncManager:
    async def __aenter__(self):
        return self
    async def __aexit__(self, *a):
        return False
try:
    with AsyncManager():
        pass
except TypeError as e:
    print(e)
