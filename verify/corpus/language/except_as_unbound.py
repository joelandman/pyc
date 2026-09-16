try:
    raise Exception()
except Exception as e:
    print("in", "e" in locals())
print("after", "e" in locals())
try:
    raise Exception()
except Exception as e:
    del e
print("del", "e" in locals())
