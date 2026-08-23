lst = [1, 2]
other = lst
lst += [3]
print(lst, other, lst is other)
tup = (1, 2)
same = tup
tup += (3,)
print(tup, same, tup is same)
