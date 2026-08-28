# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# Producer/consumer through queue.Queue, shut down with sentinels rather than a
# flag, so nothing spins. This is the shape that deadlocked when a GIL yield
# was added at loop heads (known-gaps defect E), so it is worth having gated.
import queue
import threading

q = queue.Queue(maxsize=16)
out = []
lock = threading.Lock()
CONSUMERS = 4


def produce():
    for i in range(200):
        q.put(i)
    for _ in range(CONSUMERS):
        q.put(None)


def consume():
    got = []
    while True:
        item = q.get()          # BLOCKS; never spins
        try:
            if item is None:
                break
            got.append(item * 2)
        finally:
            q.task_done()
    with lock:
        out.extend(got)


p = threading.Thread(target=produce)
cs = [threading.Thread(target=consume) for _ in range(CONSUMERS)]
p.start()
for c in cs:
    c.start()
p.join()
for c in cs:
    c.join()

print("items:", len(out))
print("sum:", sum(out))
print("first five sorted:", sorted(out)[:5])
print("queue drained:", q.empty())
