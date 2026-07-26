# relative_imports/child/child_module.py
from .. import sibling


def child_func():
    return "from child module: " + sibling.sibling_func()
