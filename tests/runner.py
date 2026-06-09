#!/usr/bin/env python3
"""Basic test runner for pyc (MVI).
Runs pyc on cases, compares stdout to python3.
"""
import subprocess, tempfile, os, sys

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
    for src, expected in CASES:
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
    print(f"{ok}/{len(CASES)}")
    sys.exit(0 if ok==len(CASES) else 1)

if __name__=="__main__":
    main()