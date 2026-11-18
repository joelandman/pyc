# pip_and_environment.md

**Date:** 2026-11-18.
**Status:** design sketch, not implemented.
**Scope:** installing applications from PyPI (and other indexes) as compiled,
self-contained Tier-1 binaries — the canonical target being `glances` via a
`pyc install glances` command — plus working inside virtual environments.

`pip` is *not* a hard requirement. The sketch drives `uv` for
dependency resolution and builds, and adds a new `pyc-install` driver that
knows how to compile with `pyc` and where to install for a Tier-1 binary.

This is a design document, so every claim below that is *measured* is tagged
(I9); claims that are *not* measured are stated as open questions, not results.

---

## 1. What is already true (measured)

These were verified against the local tree and sysroot
`$HOME/opt/py-sysroots/cp314-3.14.7-tier1` before sketching.

1. **A `pyc` build is a Tier-1 binary.** `pyc prog.py -o prog.bin` emits a
   binary that statically embeds `libpython` (it has **no** `DT_NEEDED` for
   libpython in its dynamic section; it links only `libc`/`libm`/`libz`/
   `libstdc++`/`libgcc_s`). It resolves its home at runtime from
   `$ORIGIN` / `PYTHONHOME` / `PYC_SYSROOT` and puts the sysroot's
   `lib/python3.14` **and `site-packages/`** on `sys.path`.

2. **Only what you pass to `pyc` compiles native.** A `.py` that is
   *imported* — stdlib **or** third-party — does not get native codegen. It
   runs through the embedded interpreter's real import machinery. Measured on
   this box: a 3,000,000-iteration integer loop ran in **0.28 s** when defined
   inside the compiled program and **0.27 s** when called through an `import`
   of the same function — the same order of magnitude, i.e. CPython-equivalent
   speed. There is no "compile-on-import" path and no native `.pyc` artifact
   today (a search for such a mechanism in `compiler/src/rt` and
   `compiler/src` found none; the only serialization is the `pyc_parse` AST
   envelope used by `pyc --lower`).

   Consequence, stated plainly (I9): a `pyc` binary only runs *fast* the code
   you fed it. Everything it imports runs at CPython-equivalent speed.

3. **C-extension loading already works.** The 69 built-in extensions in
   `lib/python3.14/lib-dynload/*.cpython-314-x86_64-linux-gnu.so` each link
   **only `libc`** — no libpython in `NEEDED` — and leave every `Py_*` symbol
   **unresolved** (`nm -D -u` shows `U PyArg_ParseTupleAndKeywords`, etc.).
   Those resolve from the host binary at load time, which exports them
   (`objdump -T` shows `T PyList_Append`, `T PyLong_AsLong`, …). A flat user
   `.so` built against the sysroot headers therefore behaves exactly like the
   built-ins: no second libpython in the process, symbols resolved from the
   host. **This is the enabler for C-extension dependencies.**

4. **The sysroot is C-extension-ready.** `include/python3.14/Python.h` and the
   full `Include/` set are present; a static
   `lib/python3.14/config-3.14-x86_64-linux-gnu/libpython3.14.a` exists
   (alongside `lib/libpython3.so`); `EXT_SUFFIX` is
   `.cpython-314-x86_64-linux-gnu.so`.

5. **Tooling is available.** `uv 0.12.23` is on PATH; pip 26.2.1 lives in the
   sysroot. `tools/install-pyc.sh` already lays out a prefix:
   `PREFIX/bin/pyc`, `PREFIX/lib/pyc/…`, `PREFIX/sysroots/`.

---

## 2. The honest value prop (do not oversell — I9)

For an app like `glances` (poll system stats → render a dashboard), there are
**two** real wins and one thing it is *not*:

| | Result |
|---|---|
| **Self-contained deploy** | One Tier-1 binary; **no Python interpreter, pip, or venv needed on the target.** This is the headline for a deploy-time monitoring tool. |
| **C extensions (psutil, …)** | Run in **native C**, faster than CPython — the hot polling loop is where the speed lives. |
| **Pure-Python app logic / stdlib** | Correct, **CPython-equivalent** speed (eval-stub). *Not* "compiled". State this explicitly to users. |

`pyc install` is therefore not "runs blazing fast" — it is **"deploy a
C-extension-backed app as one binary, with no Python on the target."** That is a
legitimate, distinct goal and the thing worth measuring against CPython.

---

## 3. Architecture: a `pyc-install` driver that orchestrates `uv`

`uv`/`pip` knows dependency resolution and building; it does not know how to
compile with `pyc` or where to install for a Tier-1 binary. `pyc` knows how to
compile but nothing about indexes, wheels, or transitive deps. So the driver
is the new piece; `uv` stays the resolver/builder.

```
                    ┌─────────────────────────────────────────┐
  pyc install glances ──▶  uv (resolve + fetch wheels/sdists) │
                    └─────────────────────────────────────────┘
                                   │
        ┌──────────────────────────┼──────────────────────────┐
        ▼                          ▼                          ▼
  pure-Python deps           C-extension deps          app entry point
  → install into             → rebuild FLAT .so        → emit a thin shim
    site-packages            → against sysroot hdrs     → glances.__main__:main
  (correct, eval-stub)       → no libpython NEEDED      → pyc compile → bin/
                               → install into lib-dynload             pyc-glances
                                                                  │
                                                                  ▼
                                                    Tier-1 binary, self-contained
```

**`pyc-install` is a new wrapper** (proposed at
`compiler/tools/pyc-install`, mirroring the shape of `install-pyc.sh`), with
flags roughly: `--prefix DIR`, `--venv DIR`, `--from DIR` (offline wheels),
`--sysroot`, `--upgrade`, `--index`.

---

## 4. Pipeline steps

1. **Resolve & fetch.** `uv pip download glances -d <build> --python <sysroot>
   --platform --abi --implementation`, or `uv venv` + `uv pip install` into a
   throwaway target. `uv` handles transitive deps, sdists, wheels, and cache.
2. **Pure-Python deps → `site-packages`.** Lay each wheel's `.py` into the
   target's `lib/python3.14/site-packages`; import-verify under the oracle.
   (Eval-stub at runtime — correct.)
3. **C-extension deps → flat `.so`.** Build against `include/python3.14`,
   link **only system libs** (no `-lpython`). If the builder links the shared
   libpython, make it flat with `patchelf --remove-needed libpython3.14.so`
   and set RUNPATH to the sysroot — i.e. match the built-ins. Install into
   `lib-dynload/`. Verify it loads and `Py_*` resolve from the host. **This is
   the make-or-break step.**
4. **Extract the entry point.** Read the installed dist's `entry_points.txt`
   (`glances = glances.__main__:main`). Emit a shim:
   ```python
   from glances.__main__ import main
   if __name__ == "__main__":
       main()   # returns an int exit code
   ```
   and `pyc`-compile that shim into `bin/pyc-glances`.
5. **Install & record.** Put the binary in `PREFIX/bin/pyc-glances`; write a
   manifest (name, version, pinned deps) so the install is reproducible.

---

## 5. Virtual environments

A `pyc` venv is a sysroot prefix carrying its own `site-packages` +
`lib-dynload`. The one open question is **path resolution**: the compiled
binary computes `sys.path` from the sysroot `home`, so a venv's own
`site-packages` does not appear automatically. Two options:

- **(a) Bake the venv path in at compile time** — link the app's
  `site-packages` directory into the binary as a fallback `home`/`sys.path`
  seed. Cleanest; gives real per-binary venv isolation and keeps the "no env on
  the target" property. **Preferred.**
- **(b) Point at it at runtime** — `PYTHONHOME` / `PYTHONPATH` / a new
  `PYC_SITEPACKAGES`. Simpler, less portable.

---

## 6. Risks and the one that decides feasibility

| Risk | Status |
|---|---|
| **Two libpythons in one process** | Resolved-by-design: build a flat `.so`, `Py_*` resolve from the host (proven by the 69 built-ins). |
| **Building flat (no libpython `NEEDED`)** | Main engineering effort. `patchelf` post-step, or a `--disable-shared` sysroot config, so only `libpython3.14.a` exists. |
| **sdist builds needing build deps / headers** | `uv` build isolation handles tooling; some sdists still want headers (zlib, openssl) — the sysroot must carry them. |
| **Pure-Python being eval-stub** | Acceptable; documented honestly in §2. The C-extension path is the actual speed win. |
| **venv path resolution** | Decided as §5(a): bake the site-packages into the binary at compile time. |

---

## 7. Suggested phased plan

- **M1 — driver + pure-Python.** `pyc-install` that uses `uv` to fetch and
  drops pure-Python deps into `site-packages`, and compiles a shim for a local
  package you already have. Proves the deployment end-to-end
  (glances-core + bottle, eval-stub).
- **M2 — C extensions.** The flat-`.so` build step (psutil). The hard,
  high-value step.
- **M3 — venvs + offline.** `--venv`, `--from` for air-gapped installs,
  manifest/`--upgrade`.

---

## 8. Open questions (unmeasured)

- Whether `uv`'s default C-extension link line can be made flat without a
  `patchelf` post-step, or whether a `--disable-shared` sysroot is the cleaner
  path — needs a build against psutil to answer.
- Exact set of system headers (`zlib`, `openssl`, `python3-config`) the
  sysroot must expose for the sdist builds M1/M2 will hit.
- Whether some pure-Python dependencies can be compiled by `pyc` in a future
  "native module" mode (`pyc --emit-so`) so *they* run native too, rather than
  only the entry shim. This would widen the value prop but is out of scope here.
