def r():
    try: 1/0
    except ZeroDivisionError: return "r"
print(r())
for i in [1]:
    try: 1/0
    except ZeroDivisionError: break
for i in [1,2]:
    try: 1/0
    except ZeroDivisionError: continue
def nested():
    try:
        try: 1/0
        except ZeroDivisionError: return "inner-ret"
    except Exception: return "outer"
print(nested())
# `raise ... from` is a separate recorded refusal.
raise RuntimeError("final")
