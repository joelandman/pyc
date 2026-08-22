# corpus case — ground truth is CPython at run time (CHARTER I5).
import hashlib
from hashlib import md5, sha256
import base64
import struct

print(hashlib.md5("hello world").hexdigest())
print(hashlib.sha1("hello world").hexdigest())
print(hashlib.sha256("hello world").hexdigest())
print(md5("abc").hexdigest())
print(sha256("abc").hexdigest())

def hash_of(x):
    return hashlib.sha256(x).hexdigest()
print(hash_of("param test"))

print(base64.b64encode("hello world"))
print(base64.b64encode("foo"))
print(base64.b64decode(base64.b64encode("hello world")))
print(base64.b64decode("aGVsbG8="))

packed = struct.pack("<i", 1000)
print(len(packed))
print(list(struct.unpack("<i", packed)))
print(list(struct.unpack(">HH", struct.pack(">HH", 1, 65535))))
print(list(struct.unpack("<q", struct.pack("<q", -123456789012345))))
print(list(struct.unpack("<bB", struct.pack("<bB", -5, 250))))

encoded = base64.b64encode(packed)
decoded = base64.b64decode(encoded)
print(list(struct.unpack("<i", decoded)))
