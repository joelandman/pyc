# corpus case — ground truth is CPython at run time (CHARTER I5).
x = b"hello"
print(len(x))
print(x[0])
print(x[-1])
print(list(x[1:3]))
print(x)
print(x + b" world")

ba = bytearray(b"abc")
ba.append(100)
ba.extend(b"ef")
print(ba)
ba[0] = 65
print(ba)

print(bytes())
print(bytes(3))
print(bytes([72, 105]))
print(bytes("hi", "utf-8"))

print(b"hello".hex())
print(bytes.fromhex("68656c6c6f"))
print(b"hello".decode())
print("hello".encode())

print(isinstance(b"x", bytes))
print(isinstance(bytearray(), bytearray))
print(97 in b"abc")
print(b"ab" == b"ab")
print(b"ab" < b"ac")

total = 0
for byte in b"\x01\x02\x03":
    total += byte
print(total)

print(b"\x00\x01\xff")

print(b"hello".upper())
print(b"HELLO".lower())
print(bytearray(b"hello").upper())
