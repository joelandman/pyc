# corpus case — ground truth is CPython at run time (CHARTER I5).
from collections import defaultdict, Counter

def churn_dd():
    d = defaultdict(list)
    d['seed'].append(1)
    return len(d)

def churn_ctr():
    c = Counter('aab')
    return c['a']

for i in range(5):
    churn_dd()
    churn_ctr()

plain = {}
plain['a'] = 1
print(len(plain))
try:
    plain['b']
    print('no error')
except KeyError:
    print('KeyError')
print(len(plain))

plain2 = {}
plain2['k'] = 7
try:
    print(plain2['missing'])
except KeyError:
    print('KeyError')
print(sorted(plain2.keys()))
