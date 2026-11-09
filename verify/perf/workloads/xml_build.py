# xml.etree.ElementTree build from a large, deterministic document.
#
# Perf workload (STATUS.md, real-world performance). The document is
# built in memory from fixed patterns (no file, no randomness, no
# clock); the harness records its length as the input pin. ET.fromstring
# is pure Python (the tree builder) -- the call-dense class.
import xml.etree.ElementTree as ET

ROWS = 200000

def make_doc():
    parts = ['<?xml version="1.0"?><rows>']
    for i in range(ROWS):
        parts.append(
            f'<row id="{i}" k1="{i * 3 % 97}" k2="{i * 7 % 89}" '
            f'k3="{i * 11 % 83}"><v>{i * i % 100000}</v></row>')
    parts.append("</rows>")
    return "".join(parts)

def main():
    doc = make_doc()
    root = ET.fromstring(doc)
    n = sum(1 for _ in root.iter("row"))
    print(n, len(doc))

main()
