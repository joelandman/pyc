#!/usr/bin/env python3
import os
import subprocess
import sys


def splice(arr, offset, length=None, *replacement):
    if length is None:
        length = len(arr) - offset
    removed = arr[offset:offset + length]
    arr[offset:offset + length] = list(replacement)
    return removed


# --- chop ---
s = "hello"
c = s[-1]
s = s[:-1]
print(c)   # o
print(s)   # hell

# --- warn (to stderr, should not appear on stdout) ---
sys.stderr.write("this goes to stderr\n")
print("after warn")  # after warn

# --- qw() ---
words = ["apple", "banana", "cherry"]
print(len(words))    # 3
print(words[0])      # apple
print(words[2])      # cherry

nums = ["1", "2", "3", "4", "5"]
print(",".join(nums))  # 1,2,3,4,5

# --- splice ---
arr = [1, 2, 3, 4, 5]
removed = splice(arr, 1, 2)
print(",".join(str(v) for v in arr))      # 1,4,5
print(",".join(str(v) for v in removed))  # 2,3

arr2 = [1, 2, 3, 4, 5]
splice(arr2, 1, 2, 10, 20, 30)
print(",".join(str(v) for v in arr2))  # 1,10,20,30,4,5

# --- array slice ---
data = [10, 20, 30, 40, 50]
sl = [data[1], data[3]]
print(",".join(str(v) for v in sl))  # 20,40

# --- hash slice ---
h = {"a": 1, "b": 2, "c": 3, "d": 4}
hslice = [h[k] for k in ("a", "c")]
print(",".join(str(v) for v in hslice))  # 1,3

# --- $ENV ---
os.environ["PERLC_TEST"] = "hello_env"
ev = os.environ["PERLC_TEST"]
print(ev)  # hello_env

# --- file tests ---
tmpfile = "/tmp/perlc_ft_test.txt"
with open(tmpfile, "w") as fh:
    fh.write("test\n")

print("exists" if os.path.exists(tmpfile) else "missing")   # exists
print("file" if os.path.isfile(tmpfile) else "not_file")    # file
print("dir" if os.path.isdir(tmpfile) else "not_dir")       # not_dir
print("is_dir" if os.path.isdir("/tmp") else "no_dir")      # is_dir
print("yes" if os.path.exists("/no_such_file_xyz") else "no")  # no
os.unlink(tmpfile)
print("still" if os.path.exists(tmpfile) else "gone")       # gone

# --- system ---
# Perl's system() returns the wait status; a failing command yields 256 (1<<8).
rc = subprocess.call(["true"]) << 8
print(rc)   # 0
rc2 = subprocess.call(["false"]) << 8
print(rc2)  # 256

# --- backtick ---
out = subprocess.check_output(["echo", "hello"], text=True)
out = out[:-1] if out.endswith("\n") else out
print(out)  # hello

lines = subprocess.check_output(["printf", "x\ny\nz\n"], text=True)
lines = lines[:-1] if lines.endswith("\n") else lines
lns = lines.split("\n")
print(len(lns))  # 3

print("done")
