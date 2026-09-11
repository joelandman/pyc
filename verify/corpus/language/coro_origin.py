import inspect, sys, contextlib

def here():
    info = inspect.getframeinfo(inspect.currentframe().f_back)
    return (info.filename, info.lineno)

async def corofn():
    pass

sys.set_coroutine_origin_tracking_depth(1)
fname, lineno = here()
with contextlib.closing(corofn()) as coro:
    print(coro.cr_origin)
    print(((fname, lineno + 1, "<module>"),))
sys.set_coroutine_origin_tracking_depth(0)
