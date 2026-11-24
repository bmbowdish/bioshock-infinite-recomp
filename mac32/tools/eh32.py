"""Parse i386 Mach-O __eh_frame / __gcc_except_tab (DWARF EH + Itanium LSDA). Added for BioShock Infinite.
parse(read, sections) -> (fdes, sites)
  fdes:  sorted list of (start, end)
  sites: dict func_start -> list of (lo, hi, lp, actions) where actions is a list of (filter, ti_addr, indirect);
         [] means cleanup-only. Only call sites that have a landing pad are returned."""
import struct

class R:
    def __init__(self, read, a): self.read, self.a = read, a
    def u8(self): v = self.read(self.a, 1)[0]; self.a += 1; return v
    def u16(self): v = struct.unpack("<H", self.read(self.a, 2))[0]; self.a += 2; return v
    def u32(self): v = struct.unpack("<I", self.read(self.a, 4))[0]; self.a += 4; return v
    def s32(self): v = struct.unpack("<i", self.read(self.a, 4))[0]; self.a += 4; return v
    def uleb(self):
        r = s = 0
        while True:
            b = self.u8(); r |= (b & 0x7f) << s; s += 7
            if not b & 0x80: return r
    def sleb(self):
        r = s = 0
        while True:
            b = self.u8(); r |= (b & 0x7f) << s; s += 7
            if not b & 0x80:
                if b & 0x40: r -= 1 << s
                return r
    def cstr(self):
        out = b""
        while True:
            b = self.u8()
            if b == 0: return out.decode("latin-1")
            out += bytes([b])
    def ptr(self, enc):
        """returns (value, indirect)"""
        if enc == 0xff: return None, False
        at = self.a; f = enc & 0x0f
        if f == 0x00 or f == 0x03: v = self.u32()
        elif f == 0x0b: v = self.s32()
        elif f == 0x01: v = self.uleb()
        elif f == 0x09: v = self.sleb()
        elif f == 0x02: v = self.u16()
        elif f == 0x0a: v = struct.unpack("<h", self.read(self.a - 0, 2))[0]; self.a += 2
        else: raise ValueError("ptr enc %#x" % enc)
        app = enc & 0x70
        if app == 0x10: v += at
        elif app not in (0x00,): raise ValueError("ptr app %#x" % enc)
        return v & 0xffffffff, bool(enc & 0x80)

def enc_size(enc):
    f = enc & 0x0f
    return {0x00: 4, 0x03: 4, 0x0b: 4, 0x02: 2, 0x0a: 2, 0x04: 8, 0x0c: 8}.get(f, 4)

def parse_lsda(read, lsda, fstart):
    r = R(read, lsda)
    lp_enc = r.u8()
    lpstart = r.ptr(lp_enc)[0] if lp_enc != 0xff else fstart
    tt_enc = r.u8(); ttbase = None
    if tt_enc != 0xff:
        off = r.uleb(); ttbase = r.a + off
    cs_enc = r.u8(); cs_len = r.uleb(); cs_end = r.a + cs_len
    raw = []
    while r.a < cs_end:
        s = r.ptr(cs_enc & 0x0f)[0]; l = r.ptr(cs_enc & 0x0f)[0]; lp = r.ptr(cs_enc & 0x0f)[0]; act = r.uleb()
        raw.append((s, l, lp, act))
    act_base = cs_end
    out = []
    for s, l, lp, act in raw:
        if not lp: continue
        acts = []
        if act:
            a = act_base + act - 1
            for _ in range(64):
                ar = R(read, a); flt = ar.sleb(); here = ar.a; disp = ar.sleb()
                if flt > 0 and ttbase is not None:
                    tr = R(read, ttbase - flt * enc_size(tt_enc)); v, ind = tr.ptr(tt_enc)
                    acts.append((flt, v or 0, ind))
                elif flt == 0: acts.append((0, 0, False))
                else: acts.append((flt, 0, False))   # exception specification
                if disp == 0: break
                a = here + disp
        out.append((lpstart + s, lpstart + s + l, lpstart + lp, acts))
    return out

def parse(read, eh_addr, eh_size):
    fdes, sites, cies = [], {}, {}
    a, end = eh_addr, eh_addr + eh_size
    while a < end:
        r = R(read, a); length = r.u32()
        if length == 0: break
        body = r.a; nxt = body + length
        cid = r.u32()
        if cid == 0:
            ver = r.u8(); aug = r.cstr(); r.uleb(); r.sleb()
            if ver == 1: r.u8()
            else: r.uleb()
            info = {"fde": 0, "lsda": 0xff}
            if aug.startswith("z"):
                r.uleb()
                for ch in aug[1:]:
                    if ch == "L": info["lsda"] = r.u8()
                    elif ch == "R": info["fde"] = r.u8()
                    elif ch == "P": pe = r.u8(); r.ptr(pe & 0x7f)
                    elif ch == "S": pass
            cies[a] = (aug, info)
        else:
            cie = body - cid
            aug, info = cies.get(cie, ("", {"fde": 0, "lsda": 0xff}))
            start = r.ptr(info["fde"])[0]; rng = r.ptr(info["fde"] & 0x0f)[0]
            fdes.append((start, start + rng))
            if aug.startswith("z"):
                alen = r.uleb(); adat = r.a
                if info["lsda"] != 0xff and "L" in aug:
                    lsda = r.ptr(info["lsda"])[0]
                    if lsda:
                        try:
                            s = parse_lsda(read, lsda, start)
                            if s: sites[start] = s
                        except Exception as ex:
                            print("eh32: bad LSDA for %#x: %s" % (start, ex))
        a = nxt
    fdes.sort()
    return fdes, sites
