# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# A shared counter under a Lock. The total is deterministic; the interleaving
# is not, and is never printed. Also exercises RLock reentrancy.
import threading

counter = 0
lock = threading.Lock()
rlock = threading.RLock()


def bump(n):
    global counter
    for _ in range(n):
        with lock:
            counter += 1


def reentrant():
    with rlock:
        with rlock:
            return True


threads = [threading.Thread(target=bump, args=(500,)) for _ in range(8)]
for t in threads:
    t.start()
for t in threads:
    t.join()

print("counter:", counter)
print("expected:", 8 * 500)
print("rlock reentered:", reentrant())
