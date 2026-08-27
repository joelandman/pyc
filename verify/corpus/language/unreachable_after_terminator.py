# corpus case — ground truth is CPython at run time (CHARTER I5).
#
# Statements after a terminator are unreachable. emit() already dropped their
# instructions, but make_landing_pad built pads in OTHER blocks that still
# referenced the dropped values, so the module failed to assemble with
# "use of undefined value '%vN'". Four lines of Python reproduced it, and it
# blocked Lib/test/test_compile and test_opcodes.
def after_return():
    return 3
    raise RuntimeError("unreachable")


def after_break():
    for i in range(3):
        break
        print("never")
    return i


def after_return_in_while():
    while True:
        return "w"
        x = 1 / 0


def after_raise():
    raise ValueError("boom")
    print("never")


print(after_return(), after_break(), after_return_in_while())
try:
    after_raise()
except ValueError as e:
    print("caught", e)
