# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# Compiled loops had no periodic check, so two things CPython's interpreter
# loop does never happened:
#
#   * SIGNALS NEVER RAN. `signal.alarm(1)` with `while True: n += 1` never
#     reached the handler -- and neither did Ctrl-C, so a compiled program in a
#     loop could not be interrupted at all.
#   * THREADS STARVED. A worker thread in a loop held the GIL to completion, so
#     a closure flag set by another thread was never seen and the program hung
#     forever. That is what Lib/test/test_syslog's threaded test does.
import signal
import sys
import threading
import time


def p(*a):
    print(*a, file=sys.stderr, flush=True)


def threaded_flag():
    stop = False

    def worker():
        n = 0
        while not stop:
            n += 1
        return n

    result = []
    t = threading.Thread(target=lambda: result.append(worker()), daemon=True)
    t.start()
    time.sleep(0.05)
    stop = True
    t.join(10)
    return (not t.is_alive()), bool(result)


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


p("thread saw the flag:", threaded_flag())
p("signal interrupted the loop:", signalled())
