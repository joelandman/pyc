# Test 9: Relative import used from within a package (not run directly)
# child_module.py does `from .. import sibling` internally — valid Python,
# unlike Tests 7/8 which attempt relative imports from a directly-executed
# script.
from relative_imports.child import child_module

print(child_module.child_func())
