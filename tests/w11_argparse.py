# W11.18 argparse
import argparse
p = argparse.ArgumentParser()
p.add_argument("n")
p.add_argument("--x")
ns = p.parse_args()
print(ns.n)
print(ns.x)
