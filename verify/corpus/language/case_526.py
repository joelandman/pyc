# corpus case — ground truth is CPython at run time (CHARTER I5).
import heapq
import bisect
import statistics

h = [5, 1, 8, 3, 9, 2]
h.sort()
print(h)

h2 = [5, 1, 8, 3, 9, 2]
heapq.heapify(h2)
print(h2)
heapq.heappush(h2, 0)
print(h2)
print(heapq.heappop(h2))
print(h2)
print(heapq.heappushpop(h2, 100))
print(h2)
print(heapq.heapreplace(h2, -1))
print(h2)
print(heapq.nlargest(3, [5, 1, 8, 3, 9, 2]))
print(heapq.nsmallest(3, [5, 1, 8, 3, 9, 2]))

lst = [1, 3, 5, 7, 9]
print(bisect.bisect_left(lst, 5))
print(bisect.bisect_right(lst, 5))
print(bisect.bisect(lst, 4))
bisect.insort_left(lst, 5)
print(lst)
bisect.insort(lst, 6)
print(lst)

print(statistics.mean([1, 2, 3, 4]))
print(statistics.mean([2, 4]))
print(statistics.mean([1, 2, 4]))
print(statistics.mean([1.0, 2.0, 3.0]))
print(statistics.median([1, 2, 3, 4]))
print(statistics.median([1, 2, 3]))
print(statistics.median_low([1, 2, 3, 4]))
print(statistics.median_high([1, 2, 3, 4]))
print(statistics.mode([3, 3, 1, 1, 2]))
print(statistics.mode([1, 2, 3]))
print(statistics.stdev([1, 2, 3, 4, 5]))
print(statistics.variance([1, 2, 3, 4, 5]))
print(statistics.pstdev([1, 2, 3, 4, 5]))
print(statistics.pvariance([1, 2, 3, 4, 5]))
