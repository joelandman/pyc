# Adversarial semantics for anything that keeps ints in machine registers.
# Every line here is a place a naive i64 unboxing silently diverges.
import sys

# --- overflow out of any machine word, in every operator that can do it -----
n = 1
for _ in range(70): n *= 2
print("shift out of i64:", n, n.bit_length())
print("add at the boundary:", (1 << 63) - 1 + 1, (-(1 << 63)) - 1)
print("mul overflow:", (1 << 62) * 4, (1 << 32) * (1 << 32))
print("lshift overflow:", 1 << 64, (1 << 63) << 1)
print("neg overflow:", -(-(1 << 63)))
print("abs overflow:", abs(-(1 << 63) - 1))
print("pow:", 2 ** 64, 2 ** 0, (-2) ** 63)

# --- floor division and modulo round toward NEGATIVE INFINITY, not zero -----
for a, b in ((7, 3), (-7, 3), (7, -3), (-7, -3)):
    print(f"  {a}//{b}={a//b} {a}%{b}={a%b} divmod={divmod(a, b)}")
print("big//:", (1 << 70) // 3, (-(1 << 70)) // 3)
print("mod of min:", (-(1 << 63)) % 7)

# --- shifts: negative counts raise, huge counts do not truncate -------------
try: 1 << -1
except ValueError as e: print("neg shift:", e)
print("big rshift:", (1 << 70) >> 68, 12345 >> 200, (-1) >> 200)

# --- division by zero must still raise, from the fast path too --------------
z = 0
try: 1 // z
except ZeroDivisionError as e: print("  1//0:", e)
try: 1 % z
except ZeroDivisionError as e: print("  1%0:", e)
try: divmod(1, z)
except ZeroDivisionError as e: print("  divmod(1,0):", e)
try: (1 << 70) // z
except ZeroDivisionError as e: print("  big//0:", e)

# --- bool IS an int, and must stay a bool where Python says it does ---------
t = True
print("bool arithmetic:", t + 1, t * 3, type(t + 0).__name__, type(t & t).__name__)
print("bool is int:", isinstance(True, int), True == 1, True + True)

# --- identity of small ints is OBSERVABLE ----------------------------------
a = 256; b = 256
print("small int identity:", a is b)
c = 1000; d = 1000
print("large int identity is not guaranteed, but type is:", type(c) is type(d))
print("int subclass survives:", type(True) is bool, type(1) is int)

# --- int() of a bool, and __index__ ----------------------------------------
class Idx:
    def __index__(self): return 5
print("index protocol:", [0, 1, 2, 3, 4, 5, 6][Idx()], hex(Idx().__index__()))

# --- augmented assignment on a name that is ALSO read by a closure ----------
def closure_sees_updates():
    x = 0
    def read(): return x
    x += 1
    x += (1 << 63)          # forces it out of a machine word mid-life
    return read(), x
print("closure sees:", closure_sees_updates())

# --- a local that changes TYPE mid-function --------------------------------
def type_changes(flag):
    v = 1
    if flag: v = "now a string"
    else: v += 1
    return v, type(v).__name__
print("type change:", type_changes(True), type_changes(False))

# --- exceptions must not leave a half-updated local ------------------------
def partial():
    v = 10
    try: v = v // 0
    except ZeroDivisionError: pass
    return v
print("unchanged after raise:", partial())

# --- comparison chains and mixed int/float ---------------------------------
print("mixed compare:", 1 < 1.5, (1 << 70) > 1e18, 2 ** 70 == float(2 ** 70))
print("chain:", 1 < 2 < 3, 3 > 2 > 1, 1 < 2 > 3)

# --- unboxed compare must still produce bool, not int ----------------------
def local_cmp():
    a = 3
    b = 4
    r = a < b
    n = 5
    i = 0
    s = 0
    while i < n:
        s += i
        i += 1
    return type(r).__name__, r, (r + 1), s, 1 < 2 < 3
print("local cmp:", local_cmp())

def local_for():
    n = 5
    s = 0
    for i in range(n):
        s += i
    t = 0
    for i in range(3):
        for j in range(3):
            t += i * j
    u = 0
    for i in range(3):
        u += 1 << 62
    return s, t, u
print("local for:", local_for())

def local_while_param(n):
    i = 0
    s = 0
    while i < n:
        s += i
        i += 1
    return s
print("local while param:", local_while_param(5))

def local_if():
    s = 0
    if False:
        s = 5
    s += 1
    n = 5
    i = 0
    t = 0
    u = 0
    while i < n:
        if i < 3:
            t += i
        else:
            u += i
        i += 1
    return s, t, u
print("local if:", local_if())

def local_break_cont():
    n = 5
    i = 0
    s = 0
    while i < n:
        i += 1
        if i == 2:
            continue
        if i == 4:
            break
        s += i
    t = 0
    for j in range(5):
        if j == 2:
            continue
        if j == 4:
            break
        t += j
    def early():
        u = 0
        k = 0
        while k < 5:
            if k == 3:
                return u
            u += k
            k += 1
        return u
    return s, i, t, early()
print("local break cont:", local_break_cont())
try:
    local_while_param("x")
except TypeError as e:
    print("while param str:", type(e).__name__)

# --- int with __add__ on the right (reflected ops) -------------------------
class R:
    def __radd__(self, o): return ("radd", o)
    def __rmul__(self, o): return ("rmul", o)
print("reflected:", 1 + R(), 3 * R())

# --- hash and dict keys must agree across the boundary ---------------------
big = 1 << 70
print("hash agrees:", hash(2) == hash(2.0), {2: "a"}[2.0], {big: 1}[1 << 70])

# --- sys.getsizeof / int internals still work ------------------------------
print("bit_length:", (255).bit_length(), (1 << 70).bit_length(), (0).bit_length())
print("to_bytes:", (258).to_bytes(2, "big").hex(), int.from_bytes(b"\x01\x02", "big"))
