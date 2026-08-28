# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# list.append and dict.__setitem__ from many threads. Under the GIL these are
# individually atomic; under cp314t they are made safe by the container's own
# locking rather than by a global one. Either way the FINAL CONTENTS are
# deterministic even though the order of arrival is not.
import threading

lst = []
dct = {}
st = set()
threads_n, per = 8, 250


def worker(base):
    for i in range(per):
        lst.append(base + i)
        dct[base + i] = i
        st.add((base + i) % 97)


threads = [threading.Thread(target=worker, args=(i * per,)) for i in range(threads_n)]
for t in threads:
    t.start()
for t in threads:
    t.join()

print("list length:", len(lst))
print("no items lost:", sorted(lst) == list(range(threads_n * per)))
print("dict length:", len(dct))
print("set size:", len(st))
