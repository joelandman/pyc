# Deterministic 200x200 matrix multiply (A @ B), pure Python floats.
#
# Perf workload (STATUS.md, real-world performance). Values are fixed
# expressions of the indices (no randomness, no clock); the printed sum
# is the checksum-of-work that proves the triple loop ran. The
# type-stable numeric class at a size where the work is Python-side.
N = 300

def build():
    A = [[float((i * 31 + j * 17) % 97) for j in range(N)] for i in range(N)]
    B = [[float((i * 7 + j * 23) % 89) for j in range(N)] for i in range(N)]
    return A, B

def main():
    A, B = build()
    C = [[sum(A[i][k] * B[k][j] for k in range(N)) for j in range(N)]
         for i in range(N)]
    print(f"{N}x{N} {sum(sum(r) for r in C):.6f}")

main()
