# Build pyc_lower. Same translation units as
# .github/actions/setup-pyc/action.yml — keep them in sync.
#
# CXX must be an ordinary assignment, NOT `?=`: GNU make pre-defines
# `CXX = g++` as a builtin default (see `make -p`), and a conditional
# assignment never overrides an already-defined variable. The old
# `CXX ?= clang++` was therefore dead: every local build silently used
# g++, which accepts in list-init what clang++-22 (CI) rejects as a hard
# error (lower.cpp:2780 narrowing, 2026-10-30). Local gates stayed green
# while every CI run died in "Set up pyc" — the compiler had to match,
# not just the corpus. Prefer the exact CI compiler when present; the
# command line still wins (`make CXX=clang++-18`).
HAVE_CXX22 := $(shell command -v clang++-22 >/dev/null 2>&1 && echo yes)
ifneq ($(HAVE_CXX22),)
CXX      = clang++-22
else
CXX      = clang++
endif
CXXFLAGS ?= -std=c++20 -O2 -I include
OUT      ?= /tmp/pyc_lower

SRCS := src/pyc_lower_main.cpp src/lower.cpp src/scope.cpp src/ir.cpp \
        src/codegen.cpp src/capi.cpp src/ast_support.cpp src/json.cpp

# `all` is not a file target: if $(OUT) exists as a DIRECTORY, a plain
# `all: $(OUT)` rule reads it as "already exists" and prints
# "Nothing to be done" without building (hit 2026-11-07: /tmp/pyc_lower
# was a stale empty directory, so the gate ran on a silent no-op build).
# Guard explicitly: a directory at the output path is removed, and the
# binary is rebuilt when missing or older than the newest source.
.PHONY: all clean refcount-probe
all:
	@if [ -d "$(OUT)" ] && [ ! -f "$(OUT)" ]; then \
	  echo "removing stale directory at $(OUT)"; rm -rf "$(OUT)"; \
	fi
	@if [ ! -f "$(OUT)" ] || \
	   [ "$$(ls -t $(SRCS) | head -n 1)" -nt "$(OUT)" ]; then \
	  $(CXX) $(CXXFLAGS) $(SRCS) -o $(OUT); \
	else \
	  echo "pyc_lower up to date: $(OUT)"; \
	fi

# Leak bar (B8): build the refcount harness against a Py_REF_DEBUG sysroot.
# The nightly metric job runs it; the workloads must be flat per iteration
# except `leak`, which must leak -- the negative control that proves the
# harness can fail. A release sysroot cannot count, and the probe exits 2
# with an explanation, so wiring in the wrong sysroot is a loud failure,
# not a silent pass.
DBG_SYSROOT ?= $(HOME)/opt/py-sysroots/cp314d-3.14.7-tier1
PROBE_OUT   ?= /tmp/pyc_refcount_probe

refcount-probe: src/rt/refcount_probe.cpp
	@py="$(DBG_SYSROOT)/bin/python3.14"; \
	if [ ! -x "$$py" ]; then \
	  echo "no debug sysroot at $(DBG_SYSROOT)" >&2; \
	  echo "build: tools/build-python-sysroot.sh --debug --no-pgo --no-wheel-check" >&2; \
	  exit 2; \
	fi; \
	inc="$$($$py-config --includes)"; \
	libpl="$$($$py -c 'import sysconfig;print(sysconfig.get_config_var("LIBPL"))')"; \
	lib="$$($$py -c 'import sysconfig;print(sysconfig.get_config_var("LIBRARY"))')"; \
	if [ ! -f "$$libpl/$$lib" ]; then \
	  echo "no static libpython at $$libpl/$$lib" >&2; exit 2; \
	fi; \
	$(CXX) -std=c++20 -O2 -I include -rdynamic $$inc \
	  -DPYC_PROBE_SYSROOT='"$(abspath $(DBG_SYSROOT))"' \
	  src/rt/refcount_probe.cpp \
	  -o $(PROBE_OUT) "$$libpl/$$lib" -lm -lpthread -ldl -lutil -lz && \
	echo "built $(PROBE_OUT) against $(DBG_SYSROOT)"

clean:
	rm -f $(OUT) $(PROBE_OUT)
