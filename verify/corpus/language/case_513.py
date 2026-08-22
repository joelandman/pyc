# corpus case — ground truth is CPython at run time (CHARTER I5).
import shutil
import glob
import csv
import os

src = "/tmp/pyc_test_shutil_src.txt"
dst = "/tmp/pyc_test_shutil_dst.txt"
with open(src, "w") as f:
    f.write("hello shutil")
shutil.copyfile(src, dst)
with open(dst, "r") as f:
    print(f.readlines())

dst2 = "/tmp/pyc_test_shutil_moved.txt"
shutil.move(dst, dst2)
print(sorted(glob.glob("/tmp/pyc_test_shutil_*.txt")))

os.makedirs("/tmp/pyc_test_rmtree_dir/sub", exist_ok=True)
with open("/tmp/pyc_test_rmtree_dir/f1.txt", "w") as f:
    f.write("x")
with open("/tmp/pyc_test_rmtree_dir/sub/f2.txt", "w") as f:
    f.write("y")
shutil.rmtree("/tmp/pyc_test_rmtree_dir")
print(os.path.exists("/tmp/pyc_test_rmtree_dir"))

csvpath = "/tmp/pyc_test_csv.csv"
with open(csvpath, "w") as f:
    w = csv.writer(f)
    w.writerow(["name", "age", "note"])
    w.writerow(["Alice", "30", "hello, world"])
    w.writerow(["Bob", "25", 'has "quotes"'])

with open(csvpath, "r") as f:
    lines = f.readlines()
rows = list(csv.reader(lines))
print(rows)

os.remove(src)
os.remove(dst2)
os.remove(csvpath)
