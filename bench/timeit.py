# Times each program: 2 warm-up rounds, then best of 5, interleaved. Usage: python3 timeit.py ./prog1 ./prog2 ...
import os, resource, subprocess, sys, time
progs = sys.argv[1:]
def run(p):
    t0 = time.perf_counter()
    pid = os.fork()
    if pid == 0:
        fd = os.open(os.devnull, os.O_WRONLY); os.dup2(fd, 1)
        os.execv(p, [p])
    _, status, ru = os.wait4(pid, 0)
    return (time.perf_counter() - t0) * 1000, ru.ru_maxrss / 1024
results = {}
for rnd in range(7):                       # interleave programs round by round
    for p in progs:
        ms, mb = run(p)
        if rnd >= 2:                       # first two rounds are warm-up
            results.setdefault(p, []).append((ms, mb))
for p in progs:
    best = min(r[0] for r in results[p]); mb = max(r[1] for r in results[p])
    print(f"{p:26s} {best:8.1f} ms   {mb:6.1f} MB")
