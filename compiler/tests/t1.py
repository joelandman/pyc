try:
    x = 1 / 0
except ZeroDivisionError as e:
    print("caught:", e)
print("after")
