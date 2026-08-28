# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# Coordination primitives that BLOCK: Event, Barrier, Semaphore, Condition.
# A worker waiting on an Event is the correct form of the busy-wait that
# known-gaps defect E hangs on -- Event.wait releases the GIL, a spin does not.
import threading

start = threading.Event()
order = []
lock = threading.Lock()
barrier = threading.Barrier(4)
sem = threading.Semaphore(2)
peak = [0]
live = [0]


def worker(n):
    start.wait(10)                  # blocks until released
    with sem:
        with lock:
            live[0] += 1
            peak[0] = max(peak[0], live[0])
        with lock:
            live[0] -= 1
    barrier.wait(10)                # all four must arrive
    with lock:
        order.append(n)


threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
for t in threads:
    t.start()
start.set()
for t in threads:
    t.join()

print("all ran:", sorted(order))
print("event was set:", start.is_set())
print("semaphore held the limit:", peak[0] <= 2)
print("barrier parties:", barrier.parties)
