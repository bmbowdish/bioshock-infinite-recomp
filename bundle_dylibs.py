#!/usr/bin/env python3
# Claude Fable - 2026-11-24: copy every non-system dylib the app needs (FFmpeg and its dependencies from Homebrew) into
# <app>/Contents/Frameworks and rewrite the load paths, so the app runs on a Mac without Homebrew.
import os, re, shutil, subprocess, sys
app = sys.argv[1]; exe = sys.argv[2]
fw = os.path.join(app, "Contents", "Frameworks"); os.makedirs(fw, exist_ok=True)
def deps(f):
    out = subprocess.run(["otool", "-L", f], capture_output=True, text=True).stdout.splitlines()[1:]
    return [l.strip().split(" (")[0] for l in out if l.strip()]
def is_system(p): return p.startswith("/usr/lib/") or p.startswith("/System/")
origin = {}            # bundled file -> directory its original came from
def resolve(dep, user):
    if dep.startswith("/"): return dep if os.path.exists(dep) else None
    base = os.path.basename(dep)
    cands = []
    od = origin.get(user)
    if od:
        if dep.startswith("@loader_path/"): cands.append(os.path.normpath(os.path.join(od, dep[len("@loader_path/"):])))
        cands.append(os.path.join(od, base))
    cands += ["/opt/homebrew/lib/" + base, "/usr/local/lib/" + base]
    for c in cands:
        if os.path.exists(c): return c
    return None
queue = [exe]; done = set(); copied = []; unresolved = []
while queue:
    f = queue.pop()
    if f in done: continue
    done.add(f)
    for dep in deps(f):
        if is_system(dep): continue
        base = os.path.basename(dep)
        if f != exe and base == os.path.basename(f): continue      # a library's own id line
        src = resolve(dep, f)
        if not src: unresolved.append((f, dep)); continue
        dst = os.path.join(fw, base)
        if not os.path.exists(dst):
            shutil.copy2(os.path.realpath(src), dst); os.chmod(dst, 0o755)
            origin[dst] = os.path.dirname(os.path.realpath(src))
            subprocess.run(["install_name_tool", "-id", "@rpath/" + base, dst], check=False, capture_output=True)
            copied.append(base); queue.append(dst)
        new = "@executable_path/../Frameworks/" + base
        if dep != new: subprocess.run(["install_name_tool", "-change", dep, new, f], check=False, capture_output=True)
for b in copied: subprocess.run(["codesign", "--force", "--sign", "-", os.path.join(fw, b)], capture_output=True)
r = subprocess.run(["codesign", "--force", "--sign", "-", app], capture_output=True, text=True)
left = [(f, d) for f in [exe] + [os.path.join(fw, b) for b in copied] for d in deps(f) if not is_system(d) and not d.startswith("@executable_path/") and not d.startswith("@rpath/")]
size = sum(os.path.getsize(os.path.join(fw, b)) for b in copied)
print("bundled %d libraries, %.1f MB: %s" % (len(copied), size / 1e6, " ".join(sorted(copied))))
print("unresolved:", unresolved)
print("non-system absolute paths left:", left)
print("codesign app rc=%d %s" % (r.returncode, r.stderr.strip()[:200]))
