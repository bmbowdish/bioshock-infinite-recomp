# Audit of starts.txt (Claude Fable - 2026-11-24)

## Method
- Recursive-descent disassembly of the original binary (capstone, x86-32) over the code range 0x2cd0–0x13ec120.
- Seeds: every named symbol, every address in a data segment that points into code and is a known start, and every direct `call` target found along the way. Follows conditional and direct jumps, `jmp [reg*4+table]` switch tables, stops at `ret` and at calls to no-return imports.
- Pass 2 adds the remaining starts that begin with `push ebp; mov ebp,esp`.
- Cross-check: 4.69 M instructions decoded from 95,721 entries with **0 overlap conflicts** (no instruction decoded two ways), then 5,012 more entries in pass 2, again 0 conflicts. 89.7% of code bytes covered; the rest is padding, jump tables and exception landing pads.
- Every address in starts.txt and every generated function was then classified.

## Results (starts.txt after the 01:10 fix, 57,406 entries)
| Class | Count | Meaning |
|---|---|---|
| Real function entry | 100,733 (incl. generated) | fine |
| **Mid-instruction** | **2** | 26d2ff, 26d302: inside `jne` at 0x26d2fe in GJKPenetration<TriangleV, ConvexHullV>. Same fault as the 13 removed at 01:10. |
| Interior label reached by fall-through | 20 | function is generated as chained pieces; 18 are in PhysX GJK/EPA, 1 in sub_5c01d8, 1 (4cc00a) is a real function after a no-return call (keep) |
| Interior label reached only by jumps | 825 | 813 start with a prologue = tail-call targets, legitimate; 12 are true interior labels |
| Unreached, no prologue | 70 | padding / data / landing pads; cannot split reached code |

## Real bugs found (beyond the 13 already fixed)
1. **26d2fe `jne`** (GJKPenetration<TriangleV, ConvexHullV>): piece boundary sits between the compare and its branch. Flags are per-C-function locals, so the branch always sees "not equal". Wrong result in triangle-vs-convex penetration.
2. **5c028e `jge`** (sub_5c01d8, position-independent library code): reached by `cmp edi,eax; jle 5c028e` from another piece; flags lost, so `jge` is always taken.
3. **0x229edd** `missing_func` is REACHABLE: `jae 0x229edd` at 0x22ac51 in EPA::PenetrationDepth<BigConvexHullV,...> jumps into another piece's interior. Would abort when taken (deep penetration of big convex hulls).
4. The 12 `missing_func` sites created at 01:14 are in bytes after function ends (jump-table data decoded as code): not reachable.

## Not bugs (checked)
- 1,681 `call next; pop reg` (position-independent code) sites: each target is in starts.txt, so those functions are split there; the generator handles it (`PUSH(next); F_next(c); return;`). Only sub_5c01d8 has a flag crossing.
- The other 61 empty stub functions: alignment padding before a real function.

## Proposed change (file `starts_next.txt`, NOT active yet): remove 29 entries
- mid-instruction: 26d2ff 26d302
- PhysX GJK/EPA interior labels: 215385 2153a2 2153aa 226e28 226e2b 227e92 228f20 228f7b 22ac35 22ac38 22ac46 22ac57 22ac78 243ec0 252ede 265c0d 26d2fe 287147 26d32f
- sub_5c01d8 (whole function becomes one piece): 5c0207 5c0221 5c0248 5c0279 5c028e 5c02ce 5c02d2 5c02e6
- After regen, expect `missing_func sites` to stay at the 12 unreachable ones or drop; any NEW target means a cross-function jump I missed: restore that entry.

## Limits
- Disassembly from seeds cannot prove anything about the 10% of bytes it did not reach.
- "Flag crossing" was checked at piece starts only (first flag reader before any flag writer). A piece that depends on x87 or other state is not affected (that state lives in the CPU struct).
- Separate known translator bug, not part of this change: `ldmxcsr` is ignored (recomp32.py:838); sub_c6eaa6 switches to round-toward-zero and sub_129f60 sets flush-to-zero.
