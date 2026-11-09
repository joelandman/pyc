# The Computer Language Benchmarks Game -- nqueens (counting solutions).
#
# Perf workload (STATUS.md, real-world performance). Deterministic: N is
# pinned below; counts all solutions, prints the count. The shifted-mask
# form (cols / ld / rd bitsets) is pure Python backtracking -- the
# call/combinatorial class. N=13 has 73712 solutions and runs ~0.9 s.
N = 13

def solve(n):
    limit = (1 << n) - 1

    def rec(row, cols, ld, rd):
        if row == n:
            return 1
        avail = ~(cols | ld | rd) & limit
        total = 0
        while avail:
            bit = avail & -avail
            avail -= bit
            total += rec(row + 1, cols | bit,
                         (ld | bit) << 1, (rd | bit) >> 1)
        return total

    return rec(0, 0, 0, 0)

print(solve(N))
