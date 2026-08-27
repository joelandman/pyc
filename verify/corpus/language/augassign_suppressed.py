# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# The shape this was found in: the raising augmented assignment sits inside a
# `with` whose __exit__ SUPPRESSES the exception, so the program keeps running
# and uses the freed object afterwards. That is how Lib/test/test_http_cookies
# segfaulted, via `with self.assertRaises(...): morsel |= {...}`.
class Suppress:
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return True


class Boom:
    def __ior__(self, other):
        raise ValueError("ior")


def suppressed_once():
    m = Boom()
    with Suppress():
        m |= {"a": 1}
    return type(m).__name__


def suppressed_in_loop():
    m = Boom()
    for i in range(10):
        with Suppress():
            m |= {"a": i}
    return type(m).__name__


print(suppressed_once())
print(suppressed_in_loop())
