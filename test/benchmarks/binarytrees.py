# The Computer-Language Benchmarks Game
# https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
#
# contributed by Diogo
#
# SLOWEST Python implementation from:
# https://benchmarksgame-team.pages.debian.net/benchmarksgame/program/binarytrees-python3-3.html
#
# COMMAND LINE (original):
#   python3 binarytrees.py 21
#
# PROGRAM OUTPUT (with n=10 for compiler testing):
#   stretch tree of depth 11	 check: 4095
#   1024	 trees of depth 4	 check: 31744
#   256	 trees of depth 6	 check: 32512
#   64	 trees of depth 8	 check: 32704
#   16	 trees of depth 10	 check: 32752
#   long lived tree of depth 10	 check: 2047

def tree_make(n):
    node = [None, None]
    if n == 0:
        return node
    node[0] = tree_make(n-1)
    node[1] = tree_make(n-1)
    return node


def tree_check(n):
    if n[0] is None:
        return 1
    left_result = tree_check(n[0])
    right_result = tree_check(n[1])
    result = 1 + left_result + right_result
    return result


def tree_make_and_check(n, keep_in_memory=False):
    tree = tree_make(n)
    check = tree_check(tree)
    if keep_in_memory:
        return (tree, check)
    return (None, check)


def data_iter(d, n):
    niter = 1 << (n - d + 4)
    results = [0] * niter
    for index in range(0, niter):
        tree = tree_make(d)
        check = tree_check(tree)
        results[index] = check
    c = sum(results)
    return niter, d, c


n = 10

stretch_tree = tree_make_and_check(n + 1, False)
print("stretch tree of depth %d\t check: %d" % (n+1, stretch_tree[1]))

depth_list = list(range(4, n+1, 2))
id_list = list(range(0, len(depth_list)))

for value in id_list:
    d = depth_list[value]
    niter, d, c = data_iter(d, n)
    print("%d\t trees of depth %d\t check: %d" % (niter, d, c))

longlived = tree_make_and_check(n, True)
print("long lived tree of depth %d\t check: %d" % (n, longlived[1]))
