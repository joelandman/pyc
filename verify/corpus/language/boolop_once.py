# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# gh-124285: a value whose truth a nested and/or already decided must not be
# tested again. The deciding operand is still the result. `not` consumes the
# value, and CPython does test that value a second time.

class Value:
    def __init__(self, name, truth):
        self.name = name
        self.truth = truth
        self.called = 0

    def __bool__(self):
        self.called += 1
        return self.truth

    def __repr__(self):
        return self.name


def Y(name):
    return Value(name, True)


def N(name):
    return Value(name, False)


def show(label, res, vs):
    print(label, res, [v.called for v in vs], [res is v for v in vs])


def compound():
    v = [N('a'), Y('b'), Y('c'), Y('d')]
    show('compound1', v[0] and v[1] or v[2] or v[3], v)
    v = [N('a'), N('b'), Y('c'), Y('d'), N('e')]
    show('compound2', v[0] or v[1] and v[2] or v[3] or v[4], v)


def short():
    v = [Y('a'), N('b'), Y('c')]
    show('and3', v[0] and v[1] and v[0], v)
    v = [N('a'), Y('b'), N('c')]
    show('or3', v[0] or v[1] or v[0], v)


def parenthesized():
    v = [N('a'), Y('b'), Y('c')]
    show('and_and_f', (v[0] and v[1]) and v[2], v)
    v = [Y('a'), N('b'), Y('c')]
    show('and_and_tf', (v[0] and v[1]) and v[2], v)
    v = [Y('a'), Y('b'), N('c')]
    show('and_and_tt', (v[0] and v[1]) and v[2], v)
    v = [N('a'), Y('b'), Y('c')]
    show('and_or', (v[0] and v[1]) or v[2], v)
    v = [Y('a'), Y('b'), N('c')]
    show('or_and_t', (v[0] or v[1]) and v[2], v)
    v = [N('a'), N('b'), Y('c')]
    show('or_and_f', (v[0] or v[1]) and v[2], v)
    v = [N('a'), Y('b'), Y('c'), N('d')]
    show('nest_f', ((v[0] and v[1]) or v[2]) and v[3], v)
    v = [Y('a'), Y('b'), N('c'), Y('d')]
    show('nest_t', ((v[0] and v[1]) or v[2]) and v[3], v)
    v = [Y('a'), N('b'), Y('c'), N('d'), Y('e')]
    show('deep', v[0] and ((v[1] or v[2]) and v[3]) or v[4], v)
    v = [N('a'), Y('b'), N('c')]
    show('inner_f', v[0] and (v[1] or v[2]), v)
    v = [Y('a'), N('b'), Y('c')]
    show('inner_tf', v[0] and (v[1] or v[2]), v)
    v = [Y('a'), Y('b'), N('c')]
    show('inner_tt', v[0] and (v[1] or v[2]), v)
    v = [N('a'), Y('b'), Y('c')]
    show('or_inner', v[0] or (v[1] and v[2]), v)


def predicates():
    v = [N('a'), Y('b'), Y('c'), Y('d')]
    if v[0] and v[1] or v[2] or v[3]:
        print('if1', 'then', [x.called for x in v])
    else:
        print('if1', 'else', [x.called for x in v])
    v = [N('a'), Y('b')]
    if v[0] and v[1]:
        print('if_and', 'then', [x.called for x in v])
    else:
        print('if_and', 'else', [x.called for x in v])
    v = [Y('a'), N('b')]
    if v[0] or v[1]:
        print('if_or', 'then', [x.called for x in v])
    else:
        print('if_or', 'else', [x.called for x in v])
    v = [Y('a'), Y('b'), N('c')]
    print('ifexp', 'yes' if v[0] and v[1] or v[2] else 'no', [x.called for x in v])
    v = [N('a'), Y('b')]
    print('ifexp_f', 'yes' if v[0] and v[1] else 'no', [x.called for x in v])
    v = [Y('a'), N('b')]
    n = 0
    while v[0] and v[1]:
        n += 1
    print('while_f', n, [x.called for x in v])
    steps = []

    def grab():
        b = Y('b')
        steps.append(b)
        return b

    class Toggle:
        def __init__(self):
            self.called = 0
            self.left = 2

        def __bool__(self):
            self.called += 1
            self.left -= 1
            return self.left >= 0

    t = Toggle()
    n = 0
    while t and grab():
        n += 1
    print('while_n', n, t.called, [b.called for b in steps])
    v = [N('a'), Y('b'), Y('c')]
    out = [x for x in [1] if v[0] and v[1] or v[2]]
    print('comp', out, [x.called for x in v])
    v = [Y('a'), Y('b')]
    assert v[0] and v[1]
    print('assert', [x.called for x in v])
    v = [N('a'), Y('b')]
    try:
        assert v[0] and v[1]
        print('assert_f', 'noexc', [x.called for x in v])
    except AssertionError:
        print('assert_f', 'AssertionError', [x.called for x in v])


def branch(label, cond, vs):
    if cond:
        print(label, 'then', [x.called for x in vs])
    else:
        print(label, 'else', [x.called for x in vs])


def nots():
    # Value position: CPython tests the deciding operand again.
    v = [Y('a'), N('b')]
    print('not_or_t', not (v[0] or v[1]), [x.called for x in v])
    v = [N('a'), Y('b')]
    print('not_or_f', not (v[0] or v[1]), [x.called for x in v])
    v = [N('a'), Y('b')]
    print('not_and_f', not (v[0] and v[1]), [x.called for x in v])
    # Condition position: the jump is inverted, and __bool__ runs once.
    v = [Y('a'), N('b')]
    branch('if_not', not (v[0] or v[1]), v)
    v = [N('a'), N('b')]
    branch('if_not_f', not (v[0] or v[1]), v)
    v = [N('a'), Y('b')]
    branch('if_not_ft', not (v[0] or v[1]), v)
    v = [Y('a'), Y('b')]
    branch('if_not_and_t', not (v[0] and v[1]), v)
    v = [N('a'), Y('b')]
    branch('if_not_and_f', not (v[0] and v[1]), v)
    v = [Y('a'), N('b')]
    branch('if_not_and_tf', not (v[0] and v[1]), v)
    v = [Y('a'), N('b')]
    branch('if_notnot', not not (v[0] or v[1]), v)
    v = [N('a'), N('b')]
    branch('if_notnot_f', not not (v[0] or v[1]), v)
    v = [Y('a'), N('b'), Y('c')]
    branch('if_not_or_and', not (v[0] or v[1]) and v[2], v)
    v = [N('a'), N('b'), Y('c')]
    branch('if_not_or_and_f', not (v[0] or v[1]) and v[2], v)
    v = [N('a'), N('b'), N('c')]
    branch('if_not_or_and_fc', not (v[0] or v[1]) and v[2], v)
    v = [Y('a'), Y('b'), N('c')]
    branch('if_and_not', v[0] and not (v[1] or v[2]), v)
    v = [Y('a'), N('b'), N('c')]
    branch('if_and_not_f', v[0] and not (v[1] or v[2]), v)
    v = [N('a'), Y('b')]
    branch('if_not_name', not v[0] and v[1], v)
    v = [Y('a'), N('b')]
    print('ifexp_not', 'yes' if not (v[0] or v[1]) else 'no', [x.called for x in v])
    v = [N('a'), N('b')]
    n = 0
    while not (v[0] or v[1]):
        n += 1
        break
    print('while_not', n, [x.called for x in v])
    v = [Y('a'), N('b')]
    try:
        assert not (v[0] or v[1])
        print('assert_not', 'ok', [x.called for x in v])
    except AssertionError:
        print('assert_not', 'AssertionError', [x.called for x in v])


def raises():
    class Foo:
        def __init__(self, name):
            self.name = name

        def __bool__(self):
            order.append(self.name)
            raise RuntimeError('bool')

    order = []
    try:
        print('raise_or', Foo('a') or Foo('b'))
    except RuntimeError as e:
        print('raise_or', type(e).__name__, str(e), order)
    order = []
    try:
        print('raise_mix', Foo('a') and Foo('b') or Foo('c'))
    except RuntimeError as e:
        print('raise_mix', type(e).__name__, str(e), order)


def plain():
    print('plain', 0 and 1 or 2, 1 and 0 or 3, 1 and 2 or 3)
    print('plain_list', [] or [1] and [2], [0] and [] or [3])


class Len:
    def __init__(self, name, n):
        self.name = name
        self.n = n
        self.called = 0

    def __len__(self):
        self.called += 1
        return self.n

    def __repr__(self):
        return self.name


def via_len():
    v = [Len('a', 0), Len('b', 1), Len('c', 2)]
    res = v[0] and v[1] or v[2]
    print('len', res, [x.called for x in v], res is v[2])


def side_effects():
    seen = []

    def t(tag, val):
        seen.append(tag)
        return val

    a, b, c = Y('a'), N('b'), Y('c')
    res = t('A', a) and t('B', b) or t('C', c)
    print('side', seen, res, a.called, b.called, c.called)


def walrus():
    v = [N('a'), Y('b')]
    res = (w := v[0]) and v[1] or v[0]
    print('walrus', res is w, res is v[0], w is v[0], [x.called for x in v])


def returned():
    def pick(a, b, c, d):
        return a and b or c or d

    v = [N('a'), Y('b'), Y('c'), Y('d')]
    show('ret', pick(v[0], v[1], v[2], v[3]), v)


def reused():
    # The result has to stay alive after the expression, and a second
    # evaluation has to test again.
    a, b, c = N('a'), Y('b'), Y('c')
    r = a and b or c
    print('reuse', r.name, r.called, a.called, b.called, c.called)
    print('reuse2', (a and b or c).name, a.called, b.called, c.called)


compound()
short()
parenthesized()
predicates()
nots()
raises()
plain()
via_len()
side_effects()
walrus()
returned()
reused()
