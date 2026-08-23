class It:
    def __init__(self):
        self.n = 0
    def __iter__(self):
        return self
    def __next__(self):
        self.n = self.n + 1
        if self.n > 3:
            raise StopIteration
        return self.n
for v in It():
    print(v)
print(sum(It()))
print(list(It()))
