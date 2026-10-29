# Build pyc_lower. Same translation units as
# .github/actions/setup-pyc/action.yml — keep them in sync.
CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -I include
OUT      ?= /tmp/pyc_lower

SRCS := src/pyc_lower_main.cpp src/lower.cpp src/scope.cpp src/ir.cpp \
        src/codegen.cpp src/capi.cpp src/ast_support.cpp src/json.cpp

.PHONY: all clean refcount-probe
all: $(OUT)

$(OUT): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@

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
