# corpus case — ground truth is CPython at run time (CHARTER I5).
import random

random.seed(42)
print(random.random())
print(random.random())

random.seed(42)
print(random.randint(1, 100))
print(random.randint(1, 100))

random.seed(42)
print(random.randrange(10))

random.seed(42)
print(random.uniform(1.0, 2.0))

random.seed(42)
print(random.choice([10, 20, 30, 40, 50]))

random.seed(42)
lst = [1, 2, 3, 4, 5]
random.shuffle(lst)
print(lst)
