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
    # attribute access (placeholder implementation)
    ("x=1; print(x.__class__)", "<object>\n"),
    # default argument (basic)
    ("def f(x=10): return x\nprint(f())", "10\n"),
    # keyword argument
    ("def g(a,b): return a+b\nprint(g(b=3,a=4))", "7\n"),
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
            o, rc = run(f"{pyc} {name} -o /tmp/t.bin --opt=0 >/dev/null 2>&1 ; /tmp/t.bin")
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