# `test_ssl.py` as `__main__`: nondeterministic stdout

I6 flags this file `ORACLE_UNSTABLE`. Two runs of the **same** sysroot
CPython, both **exit 0**, print different stdout. pyc is not involved.

## Reproduce

```bash
python3.14 Lib/test/test_ssl.py > /tmp/a.out 2>/tmp/a.err
python3.14 Lib/test/test_ssl.py > /tmp/b.out 2>/tmp/b.err
diff -u /tmp/a.out /tmp/b.out | head
```

Measured (3.14.7, linux x86_64): ~932–938 stdout lines; hundreds of unified-diff
lines; stderr also differs. `python3.14 Lib/test/test_ssl.py -q` does **not**
stabilize stdout.

`python -m test.regrtest test_ssl` typically has `support.verbose` off, so this
does not show up in the usual CPython gate.

## Cause

`test.support.verbose` defaults to **1** (`Lib/test/support/__init__.py`) and is
not tied to unittest `-q`. File-as-script is `unittest.main()`, so verbose
logging is on.

`ThreadedEchoServer.run` (`Lib/test/test_ssl.py` ~2476, accept loop ~2728–2749):

- `self.sock.settimeout(1.0)` then `accept()` in a loop
- on success, if verbose: `server:  new connection from` + `repr(connaddr)`
  (ephemeral port)
- on `TimeoutError`, if verbose: `connection timeout TimeoutError('timed out')`

How many 1s idle accepts fire depends on wall time vs the client tests, so the
number and placement of those lines change every run.

Also timing-dependent, if verbose:

- ~2241 `Needed %d calls to do_handshake() to establish session.`
- ~2351 `Needed %d calls to complete %s().`

`AsyncoreEchoServer` (~2831) prints `new connection from %s:%s`.

Assertions still pass. The race is only the **log**.

## Upstream?

Worth a CPython issue if they care about `python Lib/test/test_ssl.py` as a
stable script:

1. `support.verbose` stays 1 under `unittest.main()` / `-q`.
2. A background accept loop with a 1s timeout writes to stdout whenever the
   client is slow.
3. Handshake retry counts are written to stdout.

Possible fixes on their side (not ours): default verbose 0 for `__main__`, skip
the timeout print, or not print ephemeral ports / retry counts unless `-v`.

pyc will not edit CPython’s test. I6 keeps the file in the denominator as
`ORACLE_UNSTABLE`.
