import re, statistics
files = {'open.txt':0,'c7n55.txt':7.55e-9,'c48n36.txt':48.36e-9,'c114n4.txt':114.4e-9,'c431n4.txt':431.4e-9,
 'c1u036.txt':1.036e-6,'c3u68.txt':3.68e-6,'c10u4.txt':10.4e-6,'c22u7.txt':22.7e-6,'c45u9.txt':45.9e-6,
 'c96u68.txt':96.68e-6,'c263u8.txt':263.8e-6}
data = {}
for f, C in files.items():
    rows = {}
    for l in open(f):
        m = re.match(r'cap (\S+ \S+): (\d+)ms CTB (\d+) CTC (\d+).*= (\S+) F', l)
        if m: rows.setdefault(m[1], []).append((int(m[2]), int(m[3]), int(m[4]), float(m[5])))
    for rg, v in rows.items():
        v = [x for x in v[1:] if x[1] > 0]
        if not v: continue
        cyc = statistics.median(x[1] for x in v)
        cpc = statistics.median(x[2] / x[1] for x in v)
        fac = statistics.median(x[3] for x in v)
        gate = statistics.median(x[0] for x in v)
        data.setdefault(rg, []).append((C, cpc, cyc, gate, fac))
for rg, pts in data.items():
    print(f"== {rg}")
    good = [p for p in pts if p[2] >= 3 or (p[2] >= 1 and p[3] > 1000)]
    for C, cpc, cyc, gate, fac in pts:
        print(f"  C={C*1e6:10.5f}uF cpc={cpc:11.1f} cyc={cyc:5.0f} gate={gate:5d}ms fac={fac*1e6:10.5f}uF {'' if (C,cpc,cyc,gate,fac) in good else '(dropped)'}")
    nz = [p for p in good if p[0] > 0]
    if len(good) >= 2:
        xs = [p[0] for p in good]; ys = [p[1] for p in good]
        n = len(xs); mx = sum(xs)/n; my = sum(ys)/n
        b = sum((x-mx)*(y-my) for x, y in zip(xs, ys)) / sum((x-mx)**2 for x in xs)
        a = my - b*mx
        print(f"  fit: cpc = {a:.1f} + {b*1e-6:.1f}/uF")
        for C, cpc, *_ in good:
            if C: print(f"    {C*1e6:9.4f}uF -> {(cpc-a)/b*1e6:9.4f}uF  {((cpc-a)/b/C-1)*100:+6.2f}%")
