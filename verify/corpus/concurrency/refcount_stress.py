# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# The case this corpus exists for. Many threads bind and rebind references to
# the SAME objects concurrently, so every iteration is an incref/decref pair on
# a shared object. Under cp314t those are atomic and biased rather than
# GIL-protected, and pyc manipulates them directly from compiled code.
#
# A failure here is a crash or a wrong final count, not a wrong interleaving.
import threading

shared = [object() for _ in range(16)]
done = []
lock = threading.Lock()


def churn(rounds):
    local = None
    for i in range(rounds):
        local = shared[i % len(shared)]      # incref shared[i], decref previous
        holder = [local, local, local]        # three more refs
        holder.pop()                          # and one back
        del holder
    with lock:
        done.append(local is not None)


threads = [threading.Thread(target=churn, args=(2000,)) for _ in range(8)]
for t in threads:
    t.start()
for t in threads:
    t.join()

print("threads finished:", len(done))
print("all held a reference:", all(done))
print("objects still alive:", len(shared), all(o is not None for o in shared))
