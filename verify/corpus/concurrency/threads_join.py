# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# The floor: threads run, return work through a shared list, and join. Output
# is the SORTED aggregate, never the order results arrived in.
import threading

results = []
lock = threading.Lock()


def work(lo, hi):
    total = sum(range(lo, hi))
    with lock:
        results.append(total)


threads = [threading.Thread(target=work, args=(i * 1000, (i + 1) * 1000))
           for i in range(8)]
for t in threads:
    t.start()
for t in threads:
    t.join()

print("count:", len(results))
print("sum:", sum(results))
print("sorted:", sorted(results))
print("matches serial:", sum(results) == sum(range(8000)))
