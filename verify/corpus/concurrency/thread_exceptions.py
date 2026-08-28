# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# An exception inside a thread does not propagate to the joiner; it goes to
# threading.excepthook. Both facts are checked, and the hook records only the
# TYPE so the output cannot depend on a traceback's wording.
import threading

captured = []
lock = threading.Lock()


def hook(args):
    with lock:
        captured.append(args.exc_type.__name__)


threading.excepthook = hook


def raiser():
    raise ValueError("inside a thread")


def catcher():
    try:
        raise KeyError("handled inside")
    except KeyError:
        return "caught"


t = threading.Thread(target=raiser)
t.start()
t.join()

results = []
ts = [threading.Thread(target=lambda: results.append(catcher())) for _ in range(4)]
for x in ts:
    x.start()
for x in ts:
    x.join()

print("excepthook saw:", sorted(captured))
print("joiner survived:", not t.is_alive())
print("handled in-thread:", sorted(results))
