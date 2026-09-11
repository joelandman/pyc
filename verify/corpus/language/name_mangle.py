class Rat:
    __slots__ = ['_Rat__num']
    def __init__(self, n):
        self.__num = n
    def get(self):
        return self.__num
print(Rat(3).get())
