# W11.4–W11.20 remaining synthetic stdlib.
import time
print(time.strftime("%Y-%m-%d", time.gmtime(0)))
print(time.strftime("%Y", (2020, 1, 1, 0, 0, 0, 2, 1, 0)))
print(time.time() > 1000000000)
time.sleep(0)

from pathlib import Path
import os
base = "/tmp/pyc_w11_glob"
os.makedirs(base + "/sub", exist_ok=True)
with open(base + "/a.txt", "w") as f:
    f.write("a")
with open(base + "/sub/b.txt", "w") as f:
    f.write("b")
print(sorted([p.name for p in Path(base).rglob("*.txt")]))

st = os.stat(base + "/a.txt")
print(st[6] > 0)
os.environ["PYC_W11"] = "ok"
print(os.environ["PYC_W11"])

import math, cmath, operator, re
print(math.isqrt(16))
print(math.comb(5, 2))
print(cmath.phase(1+0j) == 0.0)
print(operator.abs(-7))
print(re.escape("a.b"))

import fnmatch
print(fnmatch.fnmatch("foo.txt", "*.txt"))
print(fnmatch.filter(["a.py", "b.txt"], "*.py"))

import io
s = io.StringIO()
s.write("hi")
print(s.getvalue())
b = io.BytesIO()
b.write(b"xy")
print(b.getvalue())

import shlex, filecmp
print(shlex.split("a 'b c' d"))
with open("/tmp/pyc_w11_a", "w") as f:
    f.write("same")
with open("/tmp/pyc_w11_b", "w") as f:
    f.write("same")
print(filecmp.cmp("/tmp/pyc_w11_a", "/tmp/pyc_w11_b"))

import hmac
print(hmac.new(b"k", b"m", "sha1").hexdigest())

import tomllib
print(tomllib.loads("a = 1\nb = true\nc = \"x\""))

from fractions import Fraction
f = Fraction(2, 4)
print(f.numerator, f.denominator)
print(Fraction("3/9").numerator)

import zlib
z = zlib.compress(b"hellohellohello")
print(zlib.decompress(z))

import errno, stat
print(errno.ENOENT != 0)
print(stat.S_ISREG(os.stat("/tmp/pyc_w11_a")[0]))

import pprint
print(pprint.pformat(1))

import tempfile
fd, name = tempfile.mkstemp()
print(name.startswith("/tmp/"))
os.remove(name)

import secrets
print(len(secrets.token_bytes(4)))
print(len(secrets.token_hex(4)))

import warnings, traceback
print("tb" if traceback.format_exception else "no")
