# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# A closure cell shared between threads. This is the shape behind known-gaps
# defect E, written the way that WORKS on both targets: the worker blocks on an
# Event rather than spinning on the flag, so it never needs the GIL handed to
# it mid-loop.
#
# It still proves the cell is genuinely shared -- the worker reads a value the
# main thread wrote after the thread was already running.
import threading


def run():
    shared = 0
    ready = threading.Event()
    finished = threading.Event()
    seen = []

    def worker():
        ready.wait(10)              # blocks; the write happens before this
        seen.append(shared)         # reads the ENCLOSING cell
        finished.set()

    t = threading.Thread(target=worker)
    t.start()
    shared = 42                     # written after the thread started
    ready.set()
    finished.wait(10)
    t.join(10)
    return seen, shared


seen, final = run()
print("worker read the updated cell:", seen)
print("enclosing value:", final)
