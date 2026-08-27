# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# KNOWN GAP: a compiled loop never offers the GIL, so a worker thread in a loop
# holds it to completion. A flag set by another thread is never observed and
# this program HANGS rather than finishing.
#
# CPython's interpreter loop does two things periodically: run pending signal
# handlers, and offer the GIL to a waiting thread. pyc now does the first
# (verify/corpus/language/loop_periodic.py) and not the second.
#
# The second was implemented and REVERTED, deliberately, because it trades one
# hang for another:
#
#   with PyEval_SaveThread/RestoreThread at each loop head
#     Lib/test/test_syslog     hangs -> passes in 0.2s
#     Lib/test/test_logging    passes in 23s -> DEADLOCKS
#
# The deadlock is in test_config_queue_handler, which stops a QueueListener by
# enqueueing a sentinel and joining its thread: one thread left, 0% CPU, no
# progress. Two hypotheses were tested and refuted -- that RestoreThread was
# blocking against finalisation (Py_IsFinalizing guard: no change) and that the
# yield ran without the GIL held (PyGILState_Check guard: no change). gdb cannot
# attach under this machine's ptrace_scope, so there is no stack trace and the
# mechanism is NOT established. A fix that works by changing the yield interval
# would be tuning a race, not fixing it.
#
# Ordering is the measurement that shows the starvation, and the first probe
# that missed it only checked that both lines appeared:
#
#     CPython   1. main ran ... / 2. worker finished
#     pyc       2. worker finished / 1. main ran
import sys
import threading
import time


def p(*a):
    print(*a, file=sys.stderr, flush=True)


stop = False


def worker():
    n = 0
    while not stop:
        n += 1
    p("worker saw the flag after", n > 0)


t = threading.Thread(target=worker, daemon=True)
t.start()
time.sleep(0.05)
stop = True
t.join(10)
p("worker finished:", not t.is_alive())
