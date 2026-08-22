# Local verification. Same harness and same gate as CI, so a green run here
# means the same thing a green run there does.
#
#   make -C verify fast      seconds  — language corpus, P0 gate
#   make -C verify verify    minutes  — full rescued corpus + monotonicity gate
#   make -C verify metric    long     — CPython Lib/test (the I6 number)
#   make -C verify hook      install a pre-push gate
#   make -C verify accept    adopt the current run as the new baseline

REPO    := $(abspath $(CURDIR)/..)
RUN     := $(CURDIR)/run.py
CHECK   := $(CURDIR)/check_regression.py
JOBS    ?= $(shell nproc 2>/dev/null || echo 4)
SYSROOT ?=
OUT     ?= $(CURDIR)/current.json

ifeq ($(strip $(SYSROOT)),)
ORACLE_ARGS :=
else
ORACLE_ARGS := --sysroot $(SYSROOT)
endif

.PHONY: fast verify metric accept hook clean

## Seconds. The developer inner loop: language corpus, fail only on P0.
fast:
	@$(RUN) --corpus $(CURDIR)/corpus/language \
	  --jobs $(JOBS) --run-timeout 10 --compile-timeout 60 \
	  --show 6 --fail-on P0 $(ORACLE_ARGS)

## The pre-push / PR-equivalent gate.
verify:
	@$(RUN) --corpus $(CURDIR)/corpus/language --corpus $(CURDIR)/corpus/programs \
	  --jobs $(JOBS) --run-timeout 15 --compile-timeout 90 \
	  --show 12 --json $(OUT) $(ORACLE_ARGS)
	@$(CHECK) --current $(OUT)

## The north-star number (CHARTER I6). Slow; normally left to scheduled CI.
metric:
	@$(RUN) --libtest --jobs $(JOBS) \
	  --run-timeout 30 --compile-timeout 120 \
	  --show 20 --json $(CURDIR)/libtest.json $(ORACLE_ARGS)
	@$(CHECK) --baseline $(CURDIR)/baseline-libtest.json \
	          --current $(CURDIR)/libtest.json || true

## Adopt the last run as the baseline. Requires a reason in the commit message.
accept:
	@test -f $(OUT) || { echo "no $(OUT) — run 'make verify' first"; exit 1; }
	@$(CHECK) --update --current $(OUT)

## Pre-push hook: runs the fast gate, refuses a push that introduces a P0.
hook:
	@printf '%s\n' \
	  '#!/bin/sh' \
	  '# installed by: make -C verify hook' \
	  'exec make -C "$$(git rev-parse --show-toplevel)/verify" fast' \
	  > $(REPO)/.git/hooks/pre-push
	@chmod +x $(REPO)/.git/hooks/pre-push
	@echo "installed $(REPO)/.git/hooks/pre-push (bypass with git push --no-verify)"

clean:
	@rm -f $(CURDIR)/current.json $(CURDIR)/libtest.json
