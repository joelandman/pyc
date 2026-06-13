#!/usr/bin/env python3
"""Basic test runner for pyc (MVI).
Runs pyc on cases, compares stdout to python3.
"""
import subprocess, tempfile, os, sys, shlex

CASES = [
    ("x=2+3; print(x)", "5\n"),
    ("""
def add(a,b): return a+b
x=add(2,3)
if x>0:
    print(x)
else:
    print(0)
i=0
while i<3:
    print(i)
    i=add(i,1)
print(42)
""", "5\n0\n1\n2\n42\n"),
    ("def f(a): return a+1\nprint(f(41))", "42\n"),
    # Additional cases using only currently supported features
    ("""
def add(a, b): return a + b
print(add(add(1, 2), 3))
""", "6\n"),
    ("""
    x = 1
    if x > 0:
        print(10)
    print(20)
    """, "10\n20\n"),
    # Arithmetic coverage
    ("print(5-2)", "3\n"),
    ("print(4*3)", "12\n"),
    ("print(7//2)", "3\n"),
    ("print(7%3)", "1\n"),
    ("print(2+3*4)", "14\n"),
    # list literal (basic construction test)
    ("lst=[1,2]; print(42)", "42\n"),
    # string literal
    ('print("hello")', "hello\n"),
    # break in while
    ("""
i=0
while i<5:
    if i==2: break
    print(i)
    i=i+1
""", "0\n1\n"),
    # dict literal
    ("d={'a':1}; print(99)", "99\n"),
    # tuple literal (treated as list for now)
    ("t=(1,2); print(7)", "7\n"),
    # keyword argument (parsed but not yet passed specially)
    ("def f(x=1): return x\nprint(f(x=42))", "42\n"),
    # attribute access: skipped until type objects are implemented
    # ("x=1; print(x.__class__)", "<object>\n"),
    # default argument (basic)
    ("def f(x=10): return x\nprint(f())", "10\n"),
    # keyword argument
    ("def g(a,b): return a+b\nprint(g(b=3,a=4))", "7\n"),
    # range() single arg — for loop sum
    ("s=0\nfor i in range(5):\n    s=s+i\nprint(s)", "10\n"),
    # range() two args
    ("for i in range(2,5):\n    print(i)", "2\n3\n4\n"),
    # range() three args (step)
    ("for i in range(0,10,3):\n    print(i)", "0\n3\n6\n9\n"),
    # nested for loops with range
    ("""
s=0
for i in range(3):
    for j in range(3):
        s=s+1
print(s)
""", "9\n"),
    # float literal
    ("print(3.14)", "3.14\n"),
    # float arithmetic
    ("print(1.0+2.0)", "3.0\n"),
    ("print(10.0/4.0)", "2.5\n"),
    # true division of ints returns float
    ("print(3/2)", "1.5\n"),
    # floor division of ints returns int
    ("print(7//2)", "3\n"),
    # mixed int/float arithmetic
    ("x=1.5\nprint(x+0.5)", "2.0\n"),
    # float comparison in while loop
    ("x=0.0\nwhile x<1.0:\n    x=x+0.25\nprint(x)", "1.0\n"),
    # --- string operations ---
    # concatenation
    ('print("hello" + " " + "world")', "hello world\n"),
    # repetition
    ('print("ab" * 3)', "ababab\n"),
    ('print(3 * "xy")', "xyxyxy\n"),
    # len() on string
    ('print(len("hello"))', "5\n"),
    # len() on list
    ("print(len([1,2,3]))", "3\n"),
    # str() conversions
    ("print(str(42))", "42\n"),
    ("print(str(3.14))", "3.14\n"),
    # string equality used in control flow
    ('s="hi"\nif s=="hi":\n    print(1)\nelse:\n    print(0)', "1\n"),
    # string in variable + concatenation
    ('a="foo"\nb="bar"\nprint(a+b)', "foobar\n"),
    # --- f-strings ---
    ('x=5\nprint(f"x={x}")', "x=5\n"),
    ('x=5\nprint(f"x+1={x+1}")', "x+1=6\n"),
    ('print(f"a={1}, b={2}")', "a=1, b=2\n"),
    ('print(f"pi={3.14}")', "pi=3.14\n"),
    ('name="Alice"\nprint(f"hello {name}!")', "hello Alice!\n"),
    # f-string with no interpolation
    ('print(f"plain")', "plain\n"),
    # f-string combining range loop result
    ("""
s=""
for i in range(3):
    s=s+str(i)
print(s)
""", "012\n"),
    # --- print() with multiple arguments ---
    ("print(1, 2, 3)", "1 2 3\n"),
    ('print("hello", "world")', "hello world\n"),
    ("print(1, 2.5, \"hi\")", "1 2.5 hi\n"),
    ("print()", "\n"),
    ('x=10\nprint("x =", x)', "x = 10\n"),
    ("a=1\nb=2\nprint(a+b, a*b)", "3 2\n"),
    # --- bool type ---
    ("print(True)", "True\n"),
    ("print(False)", "False\n"),
    ("print(1 < 2)", "True\n"),
    ("print(1 > 2)", "False\n"),
    ("print(1 == 1)", "True\n"),
    ("print(True + 1)", "2\n"),
    ("print(True + True)", "2\n"),
    ("print(True == True)", "True\n"),
    ("print(True == False)", "False\n"),
    ("x=True\nprint(x)", "True\n"),
    ('print(str(True))', "True\n"),
    ('print(f"val={True}")', "val=True\n"),
    # --- and / or / not ---
    ("print(True and True)",   "True\n"),
    ("print(True and False)",  "False\n"),
    ("print(False and True)",  "False\n"),
    ("print(True or False)",   "True\n"),
    ("print(False or True)",   "True\n"),
    ("print(False or False)",  "False\n"),
    ("print(not True)",  "False\n"),
    ("print(not False)", "True\n"),
    # short-circuit: return actual value, not just True/False
    ("x=0\nprint(x or 42)",  "42\n"),
    ("x=5\nprint(x and 42)", "42\n"),
    ("x=0\nprint(x and 42)", "0\n"),
    # chained
    ("print(1 and 2 and 3)",   "3\n"),
    ("print(0 or 0 or 42)",    "42\n"),
    # combined with comparisons
    ("x=5\nprint(x > 0 and x < 10)",  "True\n"),
    ("x=15\nprint(x > 0 and x < 10)", "False\n"),
    # precedence: (True and False) or 99
    ("print(True and False or 99)", "99\n"),
    # unary minus
    ("print(-5)",    "-5\n"),
    ("print(-3.14)", "-3.14\n"),
    # --- elif ---
    ("x=2\nif x==1:\n    print(1)\nelif x==2:\n    print(2)\nelse:\n    print(3)", "2\n"),
    ("x=3\nif x==1:\n    print(1)\nelif x==2:\n    print(2)\nelse:\n    print(3)", "3\n"),
    # --- subscript get ---
    ("a=[10,20,30]\nprint(a[0])\nprint(a[2])", "10\n30\n"),
    ('s="hello"\nprint(s[1])', "e\n"),
    # --- subscript set ---
    ("a=[1,2,3]\na[1]=99\nprint(a[1])", "99\n"),
    # --- dict subscript ---
    ('d={"x":42}\nprint(d["x"])', "42\n"),
    # --- augmented assignment ---
    ("x=5\nx+=3\nprint(x)", "8\n"),
    ("x=6\nx*=2\nprint(x)", "12\n"),
    ("x=10\nx-=3\nprint(x)", "7\n"),
    ("x=7\nx//=2\nprint(x)", "3\n"),
    # --- power ---
    ("print(2**10)", "1024\n"),
    ("print(3**3)",  "27\n"),
    # --- in / not in ---
    ("print(2 in [1,2,3])",   "True\n"),
    ("print(5 in [1,2,3])",   "False\n"),
    ('print("el" in "hello")', "True\n"),
    ("print(4 not in [1,2,3])", "True\n"),
    # --- ternary ---
    ("x=5\nprint(x if x>0 else -1)", "5\n"),
    ("x=-3\nprint(x if x>0 else -1)", "-1\n"),
    # --- tuple unpack ---
    ("a,b=1,2\nprint(a,b)", "1 2\n"),
    ("a,b,c=10,20,30\nprint(b)", "20\n"),
    # --- multi return ---
    ("def f():\n    return 1,2\na,b=f()\nprint(a,b)", "1 2\n"),
    # --- method calls ---
    ("a=[]\na.append(1)\na.append(2)\nprint(len(a))", "2\n"),
    ('print("hello".upper())', "HELLO\n"),
    ('print("WORLD".lower())', "world\n"),
    ('print("  hi  ".strip())', "hi\n"),
    # --- int/float/abs ---
    ('print(int("42"))',   "42\n"),
    ('print(float("3.5"))', "3.5\n"),
    ("print(abs(-7))",     "7\n"),
    ("print(abs(3.5))",    "3.5\n"),
    # --- combined: subscript + augmented + for/range ---
    ("""
a=[0,0,0]
for i in range(3):
    a[i]=i*i
print(a[0],a[1],a[2])
""", "0 1 4\n"),
    # --- dict subscript set ---
    ('d={}\nd["k"]=99\nprint(d["k"])', "99\n"),
    # --- global statement ---
    ("x=0\ndef f():\n    global x\n    x=1\nf()\nprint(x)", "1\n"),
    ("count=0\ndef inc():\n    global count\n    count=count+1\ninc()\ninc()\ninc()\nprint(count)", "3\n"),
    ("x=42\ndef f():\n    global x\n    return x\nprint(f())", "42\n"),
    # --- multi-target assign ---
    ("a=b=5\nprint(a,b)", "5 5\n"),
    ("a=b=c=0\na=1\nprint(a,b,c)", "1 0 0\n"),
    # --- aug-assign on subscript ---
    ("a=[1,2,3]\na[1]+=10\nprint(a[1])", "12\n"),
    ("a=[10,20,30]\na[0]*=3\nprint(a[0])", "30\n"),
    # --- chained comparison ---
    ("x=5\nprint(1<x<10)", "True\n"),
    ("x=15\nprint(1<x<10)", "False\n"),
    ("x=1\nprint(1<x<10)", "False\n"),
    # --- min / max ---
    ("print(min(3,1,2))", "1\n"),
    ("print(max(3,1,2))", "3\n"),
    ("print(min([5,2,8,1]))", "1\n"),
    ("print(max([5,2,8,1]))", "8\n"),
    # --- list() constructor ---
    ("a=list([1,2,3])\nprint(len(a))", "3\n"),
    # --- enumerate ---
    ("for i,v in enumerate([10,20,30]):\n    print(i,v)", "0 10\n1 20\n2 30\n"),
    # --- zip ---
    ("for a,b in zip([1,2,3],[4,5,6]):\n    print(a+b)", "5\n7\n9\n"),
    # --- list comprehension ---
    ("x=[i*2 for i in range(4)]\nprint(x[0],x[1],x[2],x[3])", "0 2 4 6\n"),
    ("x=[i for i in range(10) if i%2==0]\nprint(x[0],x[2],x[4])", "0 4 8\n"),
    ("x=[i*i for i in range(5)]\nprint(len(x),x[4])", "5 16\n"),
    # --- str % formatting ---
    ("print('%d' % 42)", "42\n"),
    ("print('%s' % 'hello')", "hello\n"),
    ("print('%.1f' % 3.14)", "3.1\n"),
    ("print('x=%d, y=%d' % (1, 2))", "x=1, y=2\n"),
    # --- dict.keys with list() ---
    ("d={'a':1,'b':2}\nprint(len(list(d.keys())))", "2\n"),
    # --- sum / sorted / any / all (builtin wiring) ---
    ("print(sum([1,2,3]))", "6\n"),
    # Use element access to avoid list repr printing differences
    ("s=sorted([3,1,2]); print(s[0],s[1],s[2])", "1 2 3\n"),
    ("print(any([0,0,1]))", "True\n"),
    ("print(all([1,1,1]))", "True\n"),
    # --- str find / count / replace (method wiring) ---
    ("print('abc'.find('b'))", "1\n"),
    ("print('abc'.find('z'))", "-1\n"),
    ("print('aaa'.count('a'))", "3\n"),
    ("print('abc'.replace('b','X'))", "aXc\n"),
    # --- slicing (get with step/negatives/str; set basic + extended) ---
    ("a=[0,1,2,3,4,5]; sl=a[1:4]; print(sl[0],sl[2])", "1 3\n"),
    ("a=[0,1,2,3,4,5]; s=a[::2]; print(s[0],s[1],s[2])", "0 2 4\n"),
    ("a=[0,1,2,3,4,5]; r=a[3:0:-1]; print(r[0],r[1],r[2])", "3 2 1\n"),
    ("a=[0,1,2,3,4,5]; r=a[::-1]; print(r[0],r[5])", "5 0\n"),
    ("a=[0,1,2,3,4,5]; r=a[-4:-1]; print(r[0],r[2])", "1 3\n"),
    ("a=[0,1,2,3,4,5]; r=a[-1:1:-1]; print(r[0],r[2])", "5 3\n"),
    ('s="abcdef"; print(s[1:4])', "bcd\n"),
    ('s="abcdef"; print(s[::2])', "ace\n"),
    ('s="abcdef"; print(s[::-1])', "fedcba\n"),
    ('s="abcdef"; print(s[5:1:-1])', "fedc\n"),
    ("b=[9,8,7,6,5]; b[1:4]=[10,20,30]; print(b[0],b[1],b[2],b[3],b[4])", "9 10 20 30 5\n"),
    ("c=[0,1,2,3,4]; c[4:1:-1]=[100,200,300]; print(c[0],c[1],c[2],c[3],c[4])", "0 1 300 200 100\n"),
    ("d=[0,1,2,3,4]; d[1:4:2]=[111,222]; print(d[0],d[1],d[2],d[3],d[4])", "0 111 2 222 4\n"),
    # --- dict comprehensions (B3) ---
    ("d={k:k*k for k in range(4)}; print(d[0],d[1],d[2],d[3])", "0 1 4 9\n"),
    ("d={i:i+10 for i in [1,2,3] if i%2==1}; print(d[1],d[3])", "11 13\n"),
    ("d={x:y for x in [1,2] for y in [10,20]}; print(d[1],d[2])", "20 20\n"),
    # --- loop type tracking / widening (A1): variable type changes across iterations or backedges ---
    ("x=0\nfor i in range(3):\n    if i==2:\n        x='done'\n    else:\n        x=i\nprint(x)", "done\n"),
    ("z=42\nfor k in range(2):\n    if k==1:\n        z='end'\n    else:\n        z=k\nprint(z)", "end\n"),
    # numeric stays numeric across backedge (no spurious widen)
    ("acc=0\nfor i in range(5):\n    acc = acc + i\nprint(acc)", "10\n"),
    # A2: visible range loop var is unboxed i64 inside the loop (numeric uses)
    # and boxed on demand for Python-visible contexts (print, list, call, etc.)
    ("s=0\nfor i in range(4):\n    s = s + i*i\nprint(s)", "14\n"),
    ("lst=[]\nfor i in range(3):\n    lst.append(i)\nprint(lst[0],lst[1],lst[2])", "0 1 2\n"),
    ("def f(x): return x+1\nr=0\nfor i in range(3):\n    r = r + f(i)\nprint(r)", "6\n"),
    # use loop var after loop (must box the final value)
    ("for i in range(3): pass\nprint(i)", "2\n"),
]

FILE_CASES = [
    ("opt_range_loop.py", []),
    ("opt_numeric_locals.py", []),
    ("opt_numeric_lists.py", []),
    ("opt_args_defaults.py", ["1"]),
    ("opt_nested_destructuring.py", []),
    ("nbody.py", ["100"]),
]

def run(cmd):
    p = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=5)
    return p.stdout, p.returncode

def main():
    # Support running from source root or from build/ directory (for make check / ctest)
    candidates = [
        os.environ.get("PYC_BINARY"),
        os.path.join(os.getcwd(), "pyc"),
        os.path.join(os.getcwd(), "build", "pyc"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "pyc"),
        "./build/pyc",
    ]
    pyc = None
    for c in candidates:
        if c and os.path.exists(c) and os.access(c, os.X_OK):
            pyc = c
            break
    if not pyc:
        pyc = "pyc"  # hope it's in PATH after install
    if not os.path.exists(pyc) or not os.access(pyc, os.X_OK):
        print("ERROR: Could not find pyc binary. Set PYC_BINARY env var or build first.")
        sys.exit(1)
    ok=0
    total=0
    for src, expected in CASES:
        total += 1
        with tempfile.NamedTemporaryFile(mode="w", suffix=".py", delete=False) as f:
            f.write(src); name=f.name
        try:
            out, _ = run(f"python3 {name}")
            # normalize for comparison (strip)
            exp = out.strip()
            o, rc = run(f"{pyc} {name} -o /tmp/t.bin --opt=0 >/dev/null 2>&1 && /tmp/t.bin")
            actual = o.strip()
            if actual == exp.strip():
                print("PASS")
                ok +=1
            else:
                print("FAIL")
                print("SRC:", src[:80].replace("\n"," "))
                print("EXP:", repr(exp.strip()))
                print("ACT:", repr(actual))
        finally:
            os.unlink(name)

    tests_dir = os.path.dirname(os.path.abspath(__file__))
    for rel_path, args in FILE_CASES:
        total += 1
        src_path = os.path.join(tests_dir, rel_path)
        quoted_src = shlex.quote(src_path)
        quoted_args = " ".join(shlex.quote(a) for a in args)
        out, _ = run(f"python3 {quoted_src} {quoted_args}")
        exp = out.strip()
        o, rc = run(f"{pyc} {quoted_src} -o /tmp/t.bin --opt=3 >/dev/null 2>&1 && /tmp/t.bin {quoted_args}")
        actual = o.strip()
        if actual == exp:
            print("PASS")
            ok += 1
        else:
            print("FAIL")
            print("SRCFILE:", rel_path)
            print("EXP:", repr(exp))
            print("ACT:", repr(actual))

    print(f"{ok}/{total}")
    sys.exit(0 if ok==total else 1)

if __name__=="__main__":
    main()
