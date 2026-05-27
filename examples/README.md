# pyc Examples

This directory contains example Python programs that work with the current MVI of pyc.

## Building an example

```bash
# From the project root
mkdir -p build && cd build
cmake ..
make -j

# Compile an example
./pyc ../examples/simple.py -o simple --opt=2

# Run it
./simple
```

## Current supported features (MVI)

- Integers and basic arithmetic (`+`)
- Functions + return
- `if` / `else`
- `while` loops
- `print`
- Top-level statements

More features (strings, lists, classes, exceptions, etc.) are planned in future phases.

## Using the runtime library

After `make install`, you can also link generated code against the runtime:

```bash
clang++ myprog.o -lpycrt -o myprog
```
