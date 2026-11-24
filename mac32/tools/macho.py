"""Load the original i386 Mach-O: sections, function symbols, import stubs."""
import bisect
import lief

INDIRECT_LOCAL = 0x80000000


class Image:
    def __init__(self, path):
        b = self.bin = lief.parse(path)
        self.segments = [(s.name, s.virtual_address, s.virtual_size, s.file_offset, s.file_size)
                         for s in b.segments if s.virtual_size and s.name != "__PAGEZERO"
                         and s.name != "__LINKEDIT"]
        self.raw = open(path, "rb").read()
        sec = {s.name: s for s in b.sections}
        self.text = sec["__text"]
        ind = b.dynamic_symbol_command
        # raw indirect symbol indices (lief resolves names; locals have no symbol)
        off = ind.indirect_symbol_offset
        import struct
        nidx = ind.nb_indirect_symbols
        raw_idx = struct.unpack_from("<%dI" % nidx, self.raw, off)
        syms = list(b.symbols)
        def name_of(i):
            v = raw_idx[i]
            if v & (INDIRECT_LOCAL | 0x40000000):
                return None
            return syms[v].name

        # classic external relocations: pointer slots dyld fills with symbol address + stored addend
        self.extrel = []
        for k in range(ind.nb_external_relocations):
            ra, info = struct.unpack_from("<iI", self.raw, ind.external_relocation_offset + 8 * k)
            if ((info >> 25) & 3) == 2 and (info >> 27) & 1:
                self.extrel.append((ra & 0xFFFFFFFF, syms[info & 0xFFFFFF].name))
        self.stubs = {}     # stub addr -> import name
        s = sec["__symbol_stub"]
        for k in range(s.size // s.reserved2):
            self.stubs[s.virtual_address + k * s.reserved2] = name_of(s.reserved1 + k)
        self.ptrs = {}      # pointer-slot addr -> import name (None = local)
        for nm in ("__nl_symbol_ptr", "__la_symbol_ptr"):
            s = sec[nm]
            for k in range(s.size // 4):
                self.ptrs[s.virtual_address + 4 * k] = name_of(s.reserved1 + k)

        # functions: every text symbol in __text
        lo, hi = self.text.virtual_address, self.text.virtual_address + self.text.size
        import subprocess
        fn = {}
        out = subprocess.run(["nm", "-n", path], capture_output=True, text=True,
                             env={"LC_ALL": "C"}).stdout
        for line in out.splitlines():
            p = line.split(" ", 2)
            if len(p) == 3 and p[1] in ("T", "t"):
                v = int(p[0], 16)
                if lo <= v < hi and v not in fn:
                    fn[v] = p[2]
        import os
        extra = os.environ.get("RECOMP_EXTRA_STARTS")
        if extra:   # extra function starts (hex per line), e.g. direct call targets in stripped regions
            for ln in open(extra):
                if ln.strip():
                    v = int(ln.split()[0], 16)
                    if lo <= v < hi and v not in fn: fn[v] = "sub_%x" % v
        self.func_addrs = sorted(fn)
        self.func_names = fn
        self.name_to_addr = {v: k for k, v in fn.items()}
        self.text_end = hi

    def read(self, addr, n):
        for name, va, vs, fo, fs in self.segments:
            if va <= addr < va + vs:
                o = addr - va
                if o >= fs:
                    return b"\0" * n
                return self.raw[fo + o: fo + o + n]
        raise KeyError(hex(addr))

    def func_end(self, addr):
        i = bisect.bisect_right(self.func_addrs, addr)
        return self.func_addrs[i] if i < len(self.func_addrs) else self.text_end

    def func_containing(self, addr):
        i = bisect.bisect_right(self.func_addrs, addr) - 1
        return self.func_addrs[i] if i >= 0 else None
