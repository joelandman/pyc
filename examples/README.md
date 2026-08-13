# pyc Examples

Small programs that compile with the current compiler. For the real
capability list see [FEATURES.md](../FEATURES.md). Open gaps:
[ISSUES.md](../ISSUES.md).

## Building an example

```bash
# From the project root
mkdir -p build && cd build
cmake ..
make -j

# Compile an example
./pyc ../examples/simple.py -o simple -O2

# Run it
./simple
```

These examples are not the feature inventory. Strings, lists, classes,
exceptions, `*args`/`**kwargs`, imports, and the synthetic stdlib all exist.

## Using the runtime library

After `make install`, you can also link generated code against the runtime:

```bash
clang++ myprog.o -lpycrt -o myprog
```
