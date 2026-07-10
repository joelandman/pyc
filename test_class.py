#!/usr/bin/env python3
# Test file to verify class implementation
class TestClass:
    def __init__(self, value):
        self.value = value
    
    def get_value(self):
        return self.value

# Create instance and call method
obj = TestClass(42)
print(obj.get_value())