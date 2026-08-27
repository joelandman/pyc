# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# Compiled loops ran no periodic check, so signal handlers never fired. A
# compiled program sitting in a loop could not be interrupted AT ALL -- not by
# SIGALRM, and not by Ctrl-C.
#
# The other half of CPython's check, offering the GIL to a waiting thread, is
# NOT done: see verify/corpus/known-gaps/thread_starvation.py for why.
import signal
import sys


def p(*a):
    print(*a, file=sys.stderr, flush=True)


def signalled():
    fired = []

    def handler(sig, frame):
        fired.append(True)
        raise KeyboardInterrupt

    old = signal.signal(signal.SIGALRM, handler)
    signal.setitimer(signal.ITIMER_REAL, 0.2)
    try:
        n = 0
        while True:
            n += 1
    except KeyboardInterrupt:
        pass
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)
    return bool(fired)


def signalled_for_loop():
    fired = []

    def handler(sig, frame):
        fired.append(True)
        raise KeyboardInterrupt

    old = signal.signal(signal.SIGALRM, handler)
    signal.setitimer(signal.ITIMER_REAL, 0.2)
    try:
        n = 0
        for _ in range(10 ** 9):
            n += 1
    except KeyboardInterrupt:
        pass
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)
    return bool(fired)


p("signal interrupted a while loop:", signalled())
p("signal interrupted a for loop:", signalled_for_loop())
