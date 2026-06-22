# Test 2: Multi-module import
import simple_module, package_a.mod_a1
print(simple_module.get_value())
print(package_a.mod_a1.hello())
