#!/usr/bin/env python3
# Claude Fable - 2026-11-24: turn a run log (logs/run_out.txt, or the app's ~/Library/Logs/BioShock3-recomp.log)
# into a short performance summary: frame rate, where frame time goes (GPU / OpenGL calls / waiting for the display),
# how even the pacing is, and what the hitches coincide with.   Usage: summarize_log.py LOGFILE [first_frame]
import re, sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else "logs/run_out.txt"
first = int(sys.argv[2]) if len(sys.argv) > 2 else 0
text = open(path, errors="replace").read().splitlines()

res = [m.group(1) for l in text for m in [re.search(r"engine resolution -> (\d+x\d+)", l)] if m]
mfx = [m.group(1) for l in text for m in [re.search(r"\[mfx\] MetalFX spatial upscaling on[^:]*: (\S+ -> \S+)", l)] if m]
perf, frames, hitches, extra, slow, shader = [], [], [], {}, [], None
frame_no = 0
for l in text:
    m = re.match(r"\[perf\] present #(\d+): ([\d.]+) ms/frame \(([\d.]+) fps\), (\d+) GL calls, (\d+) draws", l)
    if m:
        frame_no = int(m.group(1))
        perf.append((frame_no, float(m.group(2)), int(m.group(4)), int(m.group(5))))
        continue
    m = re.match(r"\[frame\] last 100 frames: interval ([\d.]+) ms \| GPU avg ([\d.]+) max ([\d.]+) ms \((\d+) samples\) \| "
                 r"GL calls ([\d.]+) ms CPU \| waiting for the display ([\d.]+) ms \| pacing: (\d+) at <=18 ms, (\d+) at "
                 r"18-27, (\d+) at 27-36, (\d+) slower", l)
    if m:
        g = m.groups()
        frames.append((frame_no, float(g[0]), float(g[1]), float(g[2]), float(g[4]), float(g[5]), [int(x) for x in g[6:10]]))
        continue
    m = re.match(r"\[hitch\] present #(\d+): ([\d.]+) ms \| shaders ([\d.]+) ms \((\d+)\) \| movie ([\d.]+) ms \| "
                 r"file I/O ([\d.]+) ms", l)
    if m:
        hitches.append((int(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4)), float(m.group(5)), float(m.group(6))))
        continue
    m = re.match(r"\[hitch\+\] present #(\d+): GL calls ([\d.]+) ms of CPU, slowest single call (\S+) ([\d.]+) ms \| waiting "
                 r"for the display ([\d.]+) ms \| GPU \(frame before\) ([\d.]+) ms", l)
    if m:
        extra[int(m.group(1))] = (float(m.group(2)), m.group(3), float(m.group(4)), float(m.group(5)), float(m.group(6)))
        continue
    m = re.match(r"\[slowcall\] present #(\d+): (\S+) ([\d.]+) ms, program (\d+) (?:linked (\d+) frames ago|link time unknown)", l)
    if m:
        slow.append((int(m.group(1)), m.group(2), float(m.group(3)), int(m.group(5)) if m.group(5) else None))
        continue
    if l.startswith("[shader] so far:"):
        shader = l

def avg(xs):
    return sum(xs) / len(xs) if xs else 0.0

print("log: %s   (%d lines)" % (path, len(text)))
print("resolution: %s%s" % (res[-1] if res else "?", "   upscaling: " + mfx[-1] if mfx else ""))
P = [p for p in perf if p[0] >= first]
F = [f for f in frames if f[0] >= first]
if P:
    ms = avg([p[1] for p in P])
    print("frames %d..%d: %.1f ms per frame = %.1f fps average   (%d draws, %d GL calls per frame)"
          % (P[0][0], P[-1][0], ms, 1000 / ms, avg([p[3] for p in P]), avg([p[2] for p in P])))
if F:
    b = [sum(f[6][i] for f in F) for i in range(4)]
    tot = sum(b) or 1
    print("where a frame's time goes (averages):")
    print("  GPU working            %5.1f ms   (highest 100-frame maximum: %.1f ms)" % (avg([f[2] for f in F]), max(f[3] for f in F)))
    print("  CPU inside OpenGL      %5.1f ms" % avg([f[4] for f in F]))
    print("  waiting for display    %5.1f ms" % avg([f[5] for f in F]))
    print("pacing (time between frames): %.0f%% within 18 ms, %.0f%% 18-27 ms, %.0f%% 27-36 ms, %.0f%% slower"
          % tuple(100.0 * x / tot for x in b))
else:
    print("no [frame] lines: this log was made by a build without the frame probe")
H = [h for h in hitches if h[0] >= first]
print("hitches (frames over 50 ms): %d, adding %.1f s of stall" % (len(H), sum(h[1] - 16.7 for h in H) / 1000))
if H:
    cause = Counter()
    for fr, dt, sh, n, mv, io in H:
        e = extra.get(fr)
        if sh > 0.5 * dt:
            c = "shader compiling"
        elif io > 0.5 * dt:
            c = "reading files"
        elif mv > 0.5 * dt:
            c = "video decoding"
        elif e and e[2] > 0.4 * dt:
            c = "one long OpenGL call (%s)" % e[1]
        elif e and e[0] > 0.5 * dt:
            c = "many OpenGL calls adding up"
        elif e and e[3] > 0.5 * dt:
            c = "waiting for the display"
        elif e:
            c = "outside OpenGL (game code or its other threads)"
        else:
            c = "not classified (no probe data)"
        cause[c] += 1
    for c, k in cause.most_common():
        print("  %4d  %s" % (k, c))
S = [s for s in slow if s[0] >= first]
if S:
    by = Counter(s[1] for s in S)
    fresh = sum(1 for s in S if s[3] is not None and s[3] <= 2)
    print("single OpenGL calls over 8 ms: %d   (%s)" % (len(S), ", ".join("%s x%d" % kv for kv in by.most_common(5))))
    print("  of those, %d used a shader program linked at most 2 frames earlier (first-use stalls)" % fresh)
if shader:
    print(shader)
