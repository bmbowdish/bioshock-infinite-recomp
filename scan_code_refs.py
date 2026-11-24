# Find function starts that are referenced only from CODE (immediates like `push 0x809ca2`), not from data tables.
# A candidate is any 4-byte value inside __text that (a) points into __text, (b) points at a `55 89 e5` prologue,
# and (c) is not already a known function start. Writes new_starts.txt (hex, one per line).
import sys, os, struct
sys.path.insert(0, os.path.expanduser("~/Downloads/bioshock-infinite-recomp/mac32/tools"))
os.environ.setdefault("RECOMP_EXTRA_STARTS", os.path.expanduser("~/Downloads/bioshock-infinite-recomp/starts.txt"))
from macho import Image

img = Image(sys.argv[1])
lo = img.text.virtual_address; hi = lo + img.text.size
text = img.read(lo, img.text.size)
known = set(img.func_addrs)
print("text %#x-%#x, known function starts: %d" % (lo, hi, len(known)))

pro = set(); i = 0
while True:
    i = text.find(b"\x55\x89\xe5", i)
    if i < 0: break
    pro.add(lo + i); i += 1
print("addresses with a 55 89 e5 prologue:", len(pro))

cand = {}
n = len(text)
for off in range(4):
    m = (n - off) // 4
    words = memoryview(text[off:off + m * 4]).cast("I")
    base = lo + off
    for k, v in enumerate(words):
        if v in pro and v not in known:
            cand.setdefault(v, base + 4 * k)
print("new candidates (referenced from code, prologue, not yet a start):", len(cand))
for a in (0x809ca2, 0x8095a4):
    print("%#x: prologue bytes? %s | already known? %s | in candidates? %s" % (a, text[a - lo:a - lo + 3].hex(), a in known, a in cand))
with open(os.path.expanduser("~/Downloads/bioshock-infinite-recomp/new_starts.txt"), "w") as f:
    for a in sorted(cand): f.write("%x\n" % a)
print("wrote new_starts.txt")
