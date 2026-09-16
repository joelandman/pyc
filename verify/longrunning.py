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
    "test_sys_settrace.py",
    "test_zipfile64.py",
    "test_zipimport.py",
})
