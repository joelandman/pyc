# corpus case — ground truth is CPython at run time (CHARTER I5).
import subprocess

path = "/tmp/pyc_test_file_write_scratch.txt"
with open(path, "w") as f:
    f.write("line1\n")
    f.write("line2\n")

out = subprocess.check_output(["wc", "-c", path])
print(int(out.split()[0]))

with open(path, "w") as f:
    f.write("replaced")
out2 = subprocess.check_output(["wc", "-c", path])
print(int(out2.split()[0]))
