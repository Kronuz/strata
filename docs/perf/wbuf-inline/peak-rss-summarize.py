import json, sys, collections
rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    arm, t, v, mb, mode, js = line.split(' ', 5)
    rows[(v, mb, mode, arm)].append(json.loads(js))
M = 1 << 20
def rng(xs, k): 
    vals = [x[k] / M for x in xs]
    return f"{min(vals):.1f}-{max(vals):.1f}" if max(vals)-min(vals) >= 0.05 else f"{vals[0]:.1f}"
print("| value, share, merges | arm | records/flush | max RSS | footprint peak | heap peak | budget peak | heap - budget |")
print("|---|---|---:|---:|---:|---:|---:|---:|")
for key in sorted(rows, key=lambda k: (int(k[0]), int(k[1]), k[2], k[3])):
    xs = rows[key]
    gap = [ (x['heap_peak'] - x['budget_peak']) / M for x in xs]
    dg = {x['digest'] for x in xs}
    print(f"| {key[0]} B, {key[1]} MiB, {'off' if key[2]=='n' else 'on'} | {key[3]} | {xs[0]['records_per_flush']:.0f} | {rng(xs,'max_rss')} | {rng(xs,'footprint_peak')} | {rng(xs,'heap_peak')} | {rng(xs,'budget_peak')} | {min(gap):.2f}-{max(gap):.2f} | {len(xs)} {'/'.join(dg)}")
