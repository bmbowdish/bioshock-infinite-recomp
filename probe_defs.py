import sys, os
sys.path.insert(0, os.path.expanduser("~/Downloads/bioshock-infinite-recomp/mac32/tools"))
from macho import Image

img = Image(sys.argv[1])
syms = {}
for s in img.bin.symbols:
    syms.setdefault(s.name, s)
names = sorted({n for n in img.ptrs.values() if n})
lo = img.text.virtual_address
hi = lo + img.text.size
secs = sorted((x.virtual_address, x.virtual_address + x.size, x.name) for x in img.bin.sections if x.size)

def secname(v):
    for a, b, n in secs:
        if a <= v < b:
            return n
    return "?"

def defined(s):
    return getattr(s, "numberof_sections", 0) > 0 and s.value != 0

d_text, d_data, undef = [], [], []
for n in names:
    s = syms.get(n)
    if s is not None and defined(s):
        (d_text if lo <= s.value < hi else d_data).append((n, s.value))
    else:
        undef.append(n)

print("ptr names total:", len(names))
print("defined in __text:", len(d_text))
print("defined in data  :", len(d_data))
print("undefined/other  :", len(undef))
s = syms.get("__ZTV11FMallocAnsi")
if s is None:
    print("__ZTV11FMallocAnsi: NOT in symtab")
else:
    print("__ZTV11FMallocAnsi: value=%#x nsect=%s section=%s" % (s.value, getattr(s, "numberof_sections", "?"), secname(s.value)))
print("--- first 25 defined data symbols ---")
for n, v in d_data[:25]:
    print("%#x %s %s" % (v, secname(v), n))
print("--- first 10 defined text symbols ---")
for n, v in d_text[:10]:
    print("%#x %s" % (v, n))
print("--- first 15 undefined ---")
for n in undef[:15]:
    print(n)
