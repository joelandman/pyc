# corpus case — ground truth is CPython at run time (CHARTER I5).
import datetime
from datetime import date, timedelta

d = datetime.date(2024, 3, 15)
print(d)
print(d.year, d.month, d.day)
print(d.isoformat())
print(d.weekday())
print(d.isoweekday())

dt = datetime.datetime(2024, 3, 15, 9, 30, 45)
print(dt)
print(dt.isoformat())
print(dt.hour, dt.minute, dt.second)

td = datetime.timedelta(days=5, hours=3)
print(td)
print(td.days, td.seconds)

td_small = datetime.timedelta(minutes=1, seconds=33)
print(td_small.total_seconds())

d2 = d + datetime.timedelta(days=10)
print(d2)
d3 = d2 - d
print(d3)
print(type(d3))

print(d < d2)
print(d == d)
print(d != d2)

d4 = date(2024, 1, 1)
td2 = timedelta(weeks=1)
print(d4 + td2)

def year_of(x):
    return x.year
def show(x):
    return str(x)
print(year_of(d))
print(show(dt))
# method calls through untyped function parameters (fixed: was returning None)
def fmt_dt(x):
    return x.isoformat()
def wd_of(x):
    return x.weekday()
def ts_of(x):
    return x.total_seconds()
print(fmt_dt(d))
print(wd_of(d))
print(ts_of(td_small))
