"""Lib/test files that did not finish under the default 30s + 2× retry.

They stay in the I6 denominator. This list only changes the run budget.
"""

TIMEOUT = 600.0

FILES = frozenset({
    "test_cmd_line.py",
    "test_compileall.py",
    "test_datetime.py",
    "test_exceptions.py",
    "test_ftplib.py",
    "test_httpservers.py",
    "test_math.py",
    "test_runpy.py",
    # test_signal's ITIMER/signal tests are wall-clock bound: the suite takes
    # ~67s under CPython AND ~66s under pyc on a 32-core box (measured
    # 2026-11-07), which straddles the 30s + 60s retry budget on the 4-core
    # runner. Not a hang; a budget artifact.
    "test_signal.py",
    "test_sys_settrace.py",
    "test_zipfile64.py",
    "test_zipimport.py",
})
