# Times each program: 2 warm-up rounds, then best of 5, interleaved. Usage: python3 timeit.py ./prog1 ./prog2 ...
# --exact-rss: measure peak memory through bin/peakrss (built from peakrss.c). Without it,
# every program shows at least Python's own ~9 MB (a forked child keeps its peak across
# exec), which only matters for programs that use less than that.
import os, resource, subprocess, sys, time
progs = sys.argv[1:]
exact = False
if progs and progs[0] == "--exact-rss":
    exact = True
    progs = progs[1:]
    helper = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin", "peakrss")
def run(p):
    if exact:
        r, w = os.pipe()
    t0 = time.perf_counter()
    pid = os.fork()
    if pid == 0:
        fd = os.open(os.devnull, os.O_WRONLY); os.dup2(fd, 1)
        if exact:
            os.dup2(w, 3); os.set_inheritable(3, True)
            os.execv(helper, [helper, p])
        os.execv(p, [p])
    _, status, ru = os.wait4(pid, 0)
    ms = (time.perf_counter() - t0) * 1000
    if exact:
        os.close(w)
        kb = int(os.read(r, 64).decode() or 0)
        os.close(r)
        return ms, kb / 1024
    return ms, ru.ru_maxrss / 1024
results = {}
for rnd in range(7):                       # interleave programs round by round
    for p in progs:
        ms, mb = run(p)
        if rnd >= 2:                       # first two rounds are warm-up
            results.setdefault(p, []).append((ms, mb))
for p in progs:
    best = min(r[0] for r in results[p]); mb = max(r[1] for r in results[p])
    print(f"{p:26s} {best:8.1f} ms   {mb:6.1f} MB")
