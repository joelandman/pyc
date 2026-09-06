# Build pyc_lower. Same translation units as
# .github/actions/setup-pyc/action.yml — keep them in sync.
CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -I include
OUT      ?= /tmp/pyc_lower

SRCS := src/pyc_lower_main.cpp src/lower.cpp src/scope.cpp src/ir.cpp \
        src/codegen.cpp src/capi.cpp src/ast_support.cpp src/json.cpp

.PHONY: all clean
all: $(OUT)

$(OUT): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@

clean:
	rm -f $(OUT)
