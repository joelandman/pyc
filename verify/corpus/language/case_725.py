data = [{"t":"pt","v":[1,2]}, {"t":"pt","v":[0,0]}, {"t":"c","v":3}, {"t":"pt","v":"xy"}]
for d in data:
    match d:
        case {"t": "pt", "v": [0, 0]}: print(d, "-> origin")
        case {"t": "pt", "v": [x, y]}: print(d, "-> point", x, y)
        case {"t": "c", "v": r}: print(d, "-> circle", r)
        case _: print(d, "-> unknown")
