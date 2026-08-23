class B:
    def __init__(self,n): self.n=n
b=B(5); calls=[]
def get(): calls.append(1); return b
get().n += 1000
print(b.n, len(calls))
d={'k':[1]}; ks=[]
def key(): ks.append(1); return 'k'
d[key()] += [2]
print(d, len(ks))
x=1; x+=2; print(x)
lst=[1,2]; lst[0]+=9; print(lst)
