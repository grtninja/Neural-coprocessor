#!/usr/bin/env python3
# MGPU latency instrument - the reader (NOT part of the add-on, never shipped).
#
# Reads PresentMon CSV captures (PresentMon runs outside the game: Windows'
# own present / display / input events, nothing loaded into the process) and
# answers one question per pair of runs: did our Reflex change latency and
# frame rate, and which way.
#
#   python latcmp.py RUN.csv                    one run: every swap chain in it
#   python latcmp.py BASE.csv TEST.csv          a pair: TEST against BASE
#   options: --bridge-a 0x...  --bridge-b 0x...   the bridge's swap chain in each
#                run (ReShade.log, "[SL4] OBJECT CHECK: bridge swapchain"): the
#                pair is then judged per role - the game's chain against the
#                game's, the bridge's (what is on screen) against the bridge's.
#                Without them, chains are paired by present count, with a warning.
#            --seed N        bootstrap seed (default 1)
#
# WHAT JUDGES LATENCY. PresentMon starts a frame's clock when the previous
# Present returns - exactly where our Sleep sits (anchor=finish). Every
# column measured from that start (display latency, GPU latency, CPU busy)
# therefore counts our Sleep as part of the frame and hides what Reflex buys:
# with Reflex the wait moves from the GPU queue to before the frame starts,
# and those columns barely move. They are printed, never judged. The verdict
# uses the game's own PC latency where it sends markers, else input to photon
# (an input event to the first frame on screen after it): neither depends on
# where the frame "starts". Input to photon needs input during the capture.
#
# Standard library only. Column names differ between PresentMon versions
# (v2: FrameTime, GPULatency, DisplayLatency, AllInputToPhotonLatency ...;
# v1: MsBetweenPresents, MsGPULatency, MsUntilDisplayed ...): every metric
# is looked up by a list of names and the header mapping is printed, so a
# missing column is visible, never guessed.
import csv, sys, math, random, statistics

METRICS = [
    # key, label, candidate column names (first match wins), lower is better?
    ('frame',   'frame time (present to present)', ['FrameTime', 'MsBetweenPresents'], True),
    ('cpubusy', 'CPU busy (frame start to Present)', ['CPUBusy', 'MsCPUBusy'], True),
    ('cpuwait', 'CPU wait (in Present + after)',     ['CPUWait', 'MsCPUWait', 'MsInPresentAPI'], True),
    ('gpulat',  'GPU latency (frame start to GPU start)', ['GPULatency', 'MsGPULatency'], True),
    ('gputime', 'GPU time',                          ['GPUTime', 'MsGPUTime'], True),
    ('displat', 'display latency (frame start to screen)', ['DisplayLatency', 'MsDisplayLatency'], True),
    ('p2d',     'Present to screen',                 ['MsUntilDisplayed'], True),
    ('rpl',     'Present to GPU done',               ['MsRenderPresentLatency'], True),
    ('in2ph',   'input to photon (any input)',       ['AllInputToPhotonLatency', 'MsAllInputToPhotonLatency'], True),
    ('click2ph','click to photon',                   ['ClickToPhotonLatency', 'MsClickToPhotonLatency'], True),
    ('pcl',     'PC latency (the game\'s markers)',  ['PCLatency', 'MsPCLatency'], True),
    ('instr',   'instrumented latency',              ['InstrumentedLatency', 'MsInstrumentedLatency'], True),
]
LATENCY_ORDER = ['pcl', 'in2ph', 'click2ph']   # the first one with values in both runs judges latency
SLEEP_INSIDE = {'cpubusy', 'gpulat', 'displat'}  # measured from the frame start: our Sleep is inside

def num(v):
    if v is None:
        return None
    v = v.strip()
    if v == '' or v.upper() == 'NA':
        return None
    try:
        x = float(v)
    except ValueError:
        return None
    return x if math.isfinite(x) else None

def load(path):
    with open(path, newline='', encoding='utf-8-sig', errors='replace') as f:
        rd = csv.reader(f)
        header = next(rd)
        rows = [r for r in rd if r]
    idx = {h.strip(): i for i, h in enumerate(header)}
    lower = {h.strip().lower(): i for i, h in enumerate(header)}
    colmap = {}
    for key, _, names, _ in METRICS:
        for n in names:
            if n in idx: colmap[key] = (n, idx[n]); break
            if n.lower() in lower: colmap[key] = (n, lower[n.lower()]); break
    def col(name):
        return idx.get(name, lower.get(name.lower()))
    c_chain, c_app, c_mode, c_disp = col('SwapChainAddress'), col('Application'), col('PresentMode'), col('DisplayedTime')
    if c_disp is None: c_disp = col('MsBetweenDisplayChange')
    chains = {}
    for r in rows:
        ch = r[c_chain] if c_chain is not None and c_chain < len(r) else '?'
        d = chains.setdefault(ch, {'app': r[c_app] if c_app is not None and c_app < len(r) else '?',
                                   'modes': {}, 'n': 0, 'displayed': 0, 'm': {k: [] for k in colmap}})
        d['n'] += 1
        if c_mode is not None and c_mode < len(r):
            d['modes'][r[c_mode]] = d['modes'].get(r[c_mode], 0) + 1
        if c_disp is not None and c_disp < len(r) and num(r[c_disp]) is not None:
            d['displayed'] += 1
        for k, (_, i) in colmap.items():
            x = num(r[i]) if i < len(r) else None
            d['m'][k].append(x)
    return header, colmap, chains

def pct(xs, p):
    if not xs: return None
    s = sorted(xs); k = (len(s) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return s[lo] if lo == hi else s[lo] + (s[hi] - s[lo]) * (k - lo)

def fps_of(frames):
    t = sum(frames)
    return 1000.0 * len(frames) / t if t > 0 else None

def low1(frames):
    # "1% low": the frame rate of the slowest 1% of frames (mean of their times)
    if len(frames) < 100: return None
    s = sorted(frames, reverse=True)[:max(1, len(frames) // 100)]
    return 1000.0 / statistics.mean(s)

def block_boot_median_diff(a, b, seed, reps=2000, block=60):
    # median(b) - median(a), moving-block bootstrap (frames are not independent)
    rnd = random.Random(seed)
    def resample(x):
        if len(x) <= block: return [rnd.choice(x) for _ in x]
        out = []
        while len(out) < len(x):
            s = rnd.randrange(0, len(x) - block + 1); out.extend(x[s:s + block])
        return out[:len(x)]
    ds = sorted(statistics.median(resample(b)) - statistics.median(resample(a)) for _ in range(reps))
    return ds[int(0.025 * reps)], ds[int(0.975 * reps) - 1]

def summary(label, d, colmap):
    fr = [x for x in d['m'].get('frame', []) if x is not None]
    print(f"  {label}: {d['n']} presents, displayed {d['displayed']} | app {d['app']} | modes {d['modes']}")
    if fr:
        print(f"    fps {fps_of(fr):.1f} (1% low {low1(fr) or float('nan'):.1f}) | frame time median {statistics.median(fr):.2f} "
              f"p95 {pct(fr, 95):.2f} p99 {pct(fr, 99):.2f} ms")
    for key, lab, _, _ in METRICS:
        if key == 'frame' or key not in colmap: continue
        xs = [x for x in d['m'][key] if x is not None]
        if not xs: print(f"    {lab:42s} no values"); continue
        note = '  (our Sleep counts inside)' if key in SLEEP_INSIDE else ''
        print(f"    {lab:42s} median {statistics.median(xs):7.2f}  p95 {pct(xs, 95):7.2f} ms  (n={len(xs)}){note}")

def roles(chains, bridge):
    # role -> chain. 'bridge' = the address given; 'game' = the busiest other chain.
    r = {}
    if bridge:
        for ch in chains:
            if ch.lower() == bridge.lower(): r['bridge'] = ch
        if 'bridge' not in r: sys.exit(f'bridge chain {bridge} is not in the capture: {list(chains)}')
        others = [c for c in chains if c != r['bridge']]
        if others: r['game'] = max(others, key=lambda c: chains[c]['n'])
        return r
    # Two chains: paired by SHAPE, never named. Tomb Raider and Dawnwalker both
    # show one chain whose Present -> GPU work done is ~20-32 ms and one at
    # ~0.2 ms. Which is the game's is not settled: on Dawnwalker the game's
    # own counter (78 fps) matched the slow-completion chain, while on Tomb
    # Raider the GPU-time rule had called that shape the bridge (PresentMon's
    # per-chain GPU time is not trustworthy with two GPUs in one process).
    def rpl_med(c):
        xs = [x for x in chains[c]['m'].get('rpl', []) if x is not None]
        return statistics.median(xs) if xs else None
    if len(chains) == 2 and all(rpl_med(c) is not None for c in chains):
        a, b = sorted(chains, key=lambda c: -rpl_med(c))
        return {'slow-completion chain': a, 'fast-completion chain': b}
    for i, ch in enumerate(sorted(chains, key=lambda c: -chains[c]['n']), 1):
        r[f'chain #{i} by present count'] = ch
    return r

def compare(role, A, B, cm_a, cm_b, seed):
    fa = [x for x in A['m'].get('frame', []) if x is not None]
    fb = [x for x in B['m'].get('frame', []) if x is not None]
    dfps = None
    if fa and fb:
        dfps = 100.0 * (fps_of(fb) - fps_of(fa)) / fps_of(fa)
        lo, hi = block_boot_median_diff(fa, fb, seed)
        print(f"  fps {fps_of(fa):.1f} -> {fps_of(fb):.1f} ({dfps:+.1f}%) | median frame time "
              f"{statistics.median(fa):.2f} -> {statistics.median(fb):.2f} ms (95% CI of the change {lo:+.2f} .. {hi:+.2f})")
    for key, lab, _, _ in METRICS:
        if key == 'frame' or key not in cm_a or key not in cm_b: continue
        xa = [x for x in A['m'][key] if x is not None]; xb = [x for x in B['m'][key] if x is not None]
        if len(xa) < 30 or len(xb) < 30: continue
        lo, hi = block_boot_median_diff(xa, xb, seed)
        ma, mb = statistics.median(xa), statistics.median(xb)
        note = '  (our Sleep counts inside: not judged)' if key in SLEEP_INSIDE else ''
        print(f"  {lab:42s} {ma:7.2f} -> {mb:7.2f} ms  change {mb - ma:+6.2f} (95% CI {lo:+.2f} .. {hi:+.2f}){note}")
    judged = None
    for key in LATENCY_ORDER:
        if key in cm_a and key in cm_b:
            xa = [x for x in A['m'][key] if x is not None]; xb = [x for x in B['m'][key] if x is not None]
            if len(xa) >= 30 and len(xb) >= 30:
                lo, hi = block_boot_median_diff(xa, xb, seed)
                judged = (key, statistics.median(xb) - statistics.median(xa), lo, hi, statistics.median(xa)); break
    if judged is None or dfps is None:
        print(f"  VERDICT [{role}]: not judged - no PC latency or input-to-photon values in both runs "
              f"(PC latency needs the game's markers; input to photon needs input during the capture).")
        return
    key, d, lo, hi, base_med = judged
    lab = next(l for k, l, _, _ in METRICS if k == key)
    # One run is one sample of a scene, a spot and a hand: a change counts only
    # if its whole 95% CI is past zero AND it is at least 5% of the base (1 ms
    # floor). Smaller moves are run-to-run noise until a repeat shows them again.
    need = max(1.0, 0.05 * base_med)
    lat_down = hi < 0 and -d >= need
    lat_up = lo > 0 and d >= need
    if lat_down:
        v = f"IMPROVEMENT: {lab} {d:+.2f} ms (CI below zero), fps {dfps:+.1f}%"
    elif lat_up:
        v = f"HARM: {lab} {d:+.2f} ms (CI above zero), fps {dfps:+.1f}%"
    elif dfps <= -5.0:
        v = f"HARM: fps {dfps:+.1f}% with no latency gain ({lab} {d:+.2f} ms, CI {lo:+.2f} .. {hi:+.2f})"
    else:
        v = (f"HARMLESS: no latency change beyond noise ({lab} {d:+.2f} ms, CI {lo:+.2f} .. {hi:+.2f}; "
             f"a change must be past zero and >= {need:.1f} ms), fps {dfps:+.1f}%")
    print(f"  VERDICT [{role}] (judged on {lab}): {v}")

def main():
    args = sys.argv[1:]; seed = 1; br = {'a': None, 'b': None}
    for side in ('a', 'b'):
        o = f'--bridge-{side}'
        if o in args: i = args.index(o); br[side] = args[i + 1]; del args[i:i + 2]
    if '--seed' in args: i = args.index('--seed'); seed = int(args[i + 1]); del args[i:i + 2]
    if len(args) not in (1, 2): sys.exit('usage: latcmp.py BASE.csv [TEST.csv] [--bridge-a 0x.. --bridge-b 0x..] [--seed N]')
    runs = [load(p) for p in args]
    for p, (hdr, colmap, chains) in zip(args, runs):
        print(f"== {p}")
        print("  columns used: " + ', '.join(f"{k}={n}" for k, (n, _) in colmap.items()))
        missing = [k for k, _, _, _ in METRICS if k not in colmap]
        if missing: print("  not in this capture: " + ', '.join(missing))
        for ch, d in sorted(chains.items(), key=lambda kv: -kv[1]['n']):
            summary(f"swap chain {ch}", d, colmap)
    if len(runs) == 1: return
    (_, cm_a, ch_a), (_, cm_b, ch_b) = runs
    if (br['a'] is None) != (br['b'] is None): sys.exit('give --bridge-a and --bridge-b together')
    ra, rb = roles(ch_a, br['a']), roles(ch_b, br['b'])
    if br['a'] is None and not ('slow-completion chain' in ra and 'slow-completion chain' in rb) and (len(ch_a) > 1 or len(ch_b) > 1):
        print("\n  WARNING: more than one swap chain and no --bridge-a/--bridge-b: chains are paired by present "
              "count, which can pair the game's chain with the bridge's. Give the [SL4] addresses.")
    for role in ra:
        if role not in rb: continue
        print(f"\n== [{role}] TEST {args[1]} {rb[role]} against BASE {args[0]} {ra[role]}")
        compare(role, ch_a[ra[role]], ch_b[rb[role]], cm_a, cm_b, seed)
    print("\n  Read it with the runs' own conditions: same spot, same settings, same input; one run is one sample.")

if __name__ == '__main__':
    main()
