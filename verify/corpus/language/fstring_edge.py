x = 3.14159
n = 2**70
s = 'é\U0001F600'
w = 8
print(f'{x:.2f}|{x!r}|{x!s}|{x!a}')
print(f'{n:,}|{n:#x}|{n:>30}')
print(f'{s!a}|{s:*^10}|{len(s)}')
print(f'{x:{w}.{2}f}|{x:0{w}.3f}')
print(f'{{literal}}|{"quoted"}|{f"{x:.1f}"}')
print(f'{x=}|{n=}|{s=!a}')
print(f'{1+2=:>6}')
print(f'{[i*i for i in range(3)]}')
print(f'{ {1: "a"}[1] }')
print(f"{'\t'}|{'\\'}")
print(f'{None}|{True}|{-0.0}|{float("nan")}')
print(f'{x:%}|{0.5:.0%}')
