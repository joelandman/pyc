# SWE

Implement one ticket. Read `rebuild/CHARTER.md` and `rebuild/AGENT_DIRECTIVES.md`
before the first edit.

- Edit only the files the ticket names.
- One writer of `compiler/src/lower.cpp`, `compiler/src/codegen.cpp`, or
  `compiler/src/rt/support.cpp` at a time.
- No hardcoded expected output. Tests compare against the sysroot CPython.
- Unsupported construct → diagnostic naming construct, line, reason. Never a
  wrong value.
- No dispatch chain on a method name or type tag (I3). Escalate instead.
