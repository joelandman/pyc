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
#
# THIS GAP IS SPECIFIC TO THE GIL TARGET. Measured against a cp314t
# free-threaded sysroot built with tools/build-python-sysroot.sh --freethreaded:
#
#     corpus, cp314t   98.84%  768/777    this probe PASSES
#     corpus, cp314    98.71%  767/777    this probe TIMES OUT
#
# and exactly one case differs between the two, this one. Zero cases fail only
# under free-threading, so pyc's ownership model survives Py_GIL_DISABLED --
# where refcounts are atomic and biased -- intact.
#
# Lib/test/test_syslog behaves the same way: hangs 3/3 on cp314, passes in 0s
# 3/3 on cp314t. The hang is a GIL artefact, not a defect in the lowering.
# Without a global lock there is nothing to yield and nothing to starve.
#
# The cost of that target is real and is why it is not the default: the same
# 2M-iteration loop takes 0.839/0.895s on cp314 and 1.046/1.063s on cp314t for
# pyc, and 0.131s vs 0.211s for CPython itself. PEP 703's documented trade.
#
# NOTE the corpus is almost entirely single-threaded. The cp314t run shows the
# ABI change does not break the foundation; it does NOT show compiled code is
# thread-safe. That needs a concurrency corpus that does not exist yet.
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
