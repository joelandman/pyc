class Base:
    def who(self):
        return "base"
class Derived(Base):
    def who(self):
        return "derived"
print(Base().who())
print(Derived().who())
print(isinstance(Derived(), Base))
