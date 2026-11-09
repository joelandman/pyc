# ast.parse + compile of a large, deterministically generated source.
#
# Perf workload (STATUS.md, real-world performance). The source is
# built in memory from fixed patterns (no file, no randomness, no
# clock), so the input is identical on every run and every machine; the
# harness records its length as the input pin. `ast.parse` is pure
# Python (call-dense: every node is a constructor call) -- the class
# where native dispatch is expected to win.
import ast

FUNCS = 400
BODY_LINES = 30

def make_source():
    chunks = []
    for i in range(FUNCS):
        lines = [f"class C{i}:"]
        for j in range(4):
            lines.append(f"    def m{j}(self, a, b):")
            for k in range(BODY_LINES):
                lines.append(f"        a = a * {k + 2} + b * {(i + j) % 7 + 1}")
            lines.append("        return a + b")
        chunks.append("\n".join(lines))
    return "\n\n".join(chunks) + "\n"

def main():
    src = make_source()
    tree = ast.parse(src)
    code = compile(tree, "gen", "exec")
    print(len(tree.body), len(src))

main()
