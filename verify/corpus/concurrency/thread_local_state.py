# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# threading.local: each thread sees only its own value. The aggregate is
# deterministic; which thread got which slot is not printed.
import threading

tl = threading.local()
seen = []
lock = threading.Lock()


def worker(n):
    tl.value = n * 10
    tl.tag = f"t{n}"
    total = tl.value + len(tl.tag)
    with lock:
        seen.append((n, total, hasattr(tl, "value")))


threads = [threading.Thread(target=worker, args=(i,)) for i in range(6)]
for t in threads:
    t.start()
for t in threads:
    t.join()

print("per-thread values:", sorted(seen))
print("main thread isolated:", not hasattr(tl, "value"))
