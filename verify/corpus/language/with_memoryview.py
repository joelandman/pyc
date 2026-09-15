m = memoryview(bytearray(b"ab"))
with m:
    print(bytes(m))
print("ok")
