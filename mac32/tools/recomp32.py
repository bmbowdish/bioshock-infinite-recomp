#!/usr/bin/env python3
"""Static recompiler: i386 Mach-O functions -> C operating on the guest CPU state.

usage: recomp32.py BINARY LO HI OUTDIR [--hle FILE]   (hex addresses; translates functions in [LO,HI))
"""
import os, re, re, sys
sys.path.insert(0, os.path.dirname(__file__))
from macho import Image
import capstone as cs
from capstone import x86 as X

R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
R16 = {r[1:]: r for r in R32}
R8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
R8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
UT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
ST = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}
MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}

CC = {"e": "zf", "ne": "!zf", "b": "cf", "ae": "!cf", "be": "(cf|zf)", "a": "(!cf&&!zf)",
      "l": "(sf!=of)", "ge": "(sf==of)", "le": "(zf||sf!=of)", "g": "(!zf&&sf==of)",
      "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf", "o": "of", "no": "!of"}

STUB_SPECIAL = {"_setjmp"}


def cname(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


class Unsupported(Exception):
    pass


class FuncGen:
    def __init__(self, tr, addr):
        self.tr, self.img, self.addr = tr, tr.img, addr
        self.end = self.img.func_end(addr)
        self.code = self.img.read(addr, self.end - addr)
        # C++ landing pads (catch / cleanup blocks) are only reached by the unwinder: make them entry points too
        eh = tr.eh_sites.get(addr, [])
        self.lps = sorted(set(lp for _, _, lp, _ in eh if addr <= lp < self.end))
        tr.eh_lost += sum(1 for _, _, lp, _ in eh if not (addr <= lp < self.end))
        self.ins = self.descend()
        self.addrs = {i.address for i in self.ins}
        self.labels = set()
        self.out = []
        self.reg = None

    # ---------- operands ----------
    def rn(self, r):
        return self.cur.reg_name(r)

    def mem_addr(self, op):
        m = op.mem
        if m.segment not in (0, X.X86_REG_ES, X.X86_REG_DS):
            raise Unsupported("segment " + self.rn(m.segment))
        parts = []
        if m.base:
            parts.append("C." + self.rn(m.base))
        if m.index:
            parts.append("C.%s*%du" % (self.rn(m.index), m.scale))
        if m.disp or not parts:
            parts.append("0x%Xu" % (m.disp & 0xFFFFFFFF))
        return "(uint32_t)(%s)" % " + ".join(parts)

    def rd(self, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            n = self.rn(op.reg)
            if n in R32: return "C." + n
            if n in R16: return "(uint16_t)C." + R16[n]
            if n in R8L: return "(uint8_t)C." + R8L[n]
            if n in R8H: return "(uint8_t)(C.%s>>8)" % R8H[n]
            raise Unsupported("reg " + n)
        if op.type == X.X86_OP_IMM:
            return "(%s)0x%Xu" % (UT[size], op.imm & ((1 << (8 * size)) - 1))
        if op.type == X.X86_OP_MEM:
            return "rd%d(%s)" % (8 * size, self.mem_addr(op))
        raise Unsupported("op")

    def wr(self, op, v):
        if op.type == X.X86_OP_REG:
            n = self.rn(op.reg)
            if n in R32: return "C.%s = (uint32_t)(%s);" % (n, v)
            if n in R16: return "C.{0} = (C.{0} & 0xFFFF0000u) | (uint16_t)({1});".format(R16[n], v)
            if n in R8L: return "C.{0} = (C.{0} & 0xFFFFFF00u) | (uint8_t)({1});".format(R8L[n], v)
            if n in R8H: return "C.{0} = (C.{0} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({1}) << 8);".format(R8H[n], v)
            raise Unsupported("wreg " + n)
        if op.type == X.X86_OP_MEM:
            return "wr%d(%s, (%s)(%s));" % (8 * op.size, self.mem_addr(op), UT[op.size], v)
        raise Unsupported("wop")

    def xmm(self, op):
        n = self.rn(op.reg)
        if not n.startswith("xmm"): raise Unsupported(n)
        return "C.xmm[%s]" % n[3:]

    def e(self, s):
        self.out.append("    " + s)

    def szf(self, r, size):
        return "zf = ((%s) & %s) == 0; sf = ((%s) >> %d) & 1;" % (r, MASK[size], r, 8 * size - 1)

    # ---------- function ----------
    def decode(self, a):
        if not (self.addr <= a < self.end): return None
        for i in self.tr.md.disasm(self.code[a - self.addr:a - self.addr + 16], a, 1):
            return i
        return None

    def table_limit(self, k, ins_list, reg):
        # look back for "cmp reg, N" guarding the table -> N+1 entries
        for j in range(k - 1, max(-1, k - 6), -1):
            p = ins_list[j]
            if p.mnemonic == "cmp" and len(p.operands) == 2 and p.operands[1].type == X.X86_OP_IMM \
                    and p.operands[0].type == X.X86_OP_REG and p.operands[0].reg == reg:
                return p.operands[1].imm + 1
        return None

    def pic_specific(self, ld, seen, B):
        """Follow the table register back to the lea [B + disp] that produced it (directly, or through a stack slot).
        Returns a list of (lea disp) values; empty if the data flow is not recognised."""
        T = ld.operands[1].mem.base
        if not T: return []
        allins = sorted((x for x in seen.values() if x), key=lambda x: x.address)
        def writes(q, reg):
            return q.operands and q.operands[0].type == X.X86_OP_REG and q.operands[0].reg == reg and q.mnemonic in ("mov", "lea")
        def lea_of(reg, before):   # nearest lea reg,[B+disp] before address 'before' (within 48 bytes, no other write)
            for q in reversed([x for x in allins if before - 48 <= x.address < before]):
                if writes(q, reg):
                    if q.mnemonic == "lea" and q.operands[1].type == X.X86_OP_MEM and q.operands[1].mem.base == B \
                            and not q.operands[1].mem.index:
                        return q.operands[1].mem.disp
                    return None
            return None
        for q in reversed([x for x in allins if ld.address - 48 <= x.address < ld.address]):
            if not writes(q, T): continue
            if q.mnemonic == "lea":
                d = lea_of(T, ld.address)
                return [d] if d is not None else []
            m = q.operands[1]
            if m.type == X.X86_OP_MEM and not m.mem.index and m.mem.base in (X.X86_REG_EBP, X.X86_REG_ESP):
                out = []
                for st in allins:   # every store to that same stack slot
                    if st.mnemonic == "mov" and len(st.operands) == 2 and st.operands[0].type == X.X86_OP_MEM \
                            and st.operands[0].mem.base == m.mem.base and st.operands[0].mem.disp == m.mem.disp \
                            and not st.operands[0].mem.index and st.operands[1].type == X.X86_OP_REG:
                        d = lea_of(st.operands[1].reg, st.address)
                        if d is not None: out.append(d)
                return out
            return []
        return []

    def pic_table(self, j, seen):
        """PIC switch (Mach-O i386):  mov R,[T + X*4(+d)] ; add R,B ; jmp R   where B holds the address of a
        'pop B' (call/pop idiom) and the table holds offsets from B. Added for BioShock Infinite (Wwise audio thread)."""
        R = j.operands[0].reg
        prev = sorted((x for x in seen.values() if x and j.address - 64 <= x.address < j.address), key=lambda x: x.address)
        if len(prev) < 2: return None
        def writes_reg(q, r):
            return q.operands and q.operands[0].type == X.X86_OP_REG and q.operands[0].reg == r
        k = len(prev) - 1   # skip unrelated instructions between "add R,B" and "jmp R" (e.g. an SSE load)
        while k > 0 and len(prev) - k <= 4 and not writes_reg(prev[k], R): k -= 1
        if k < 1: return None
        add, ld = prev[k], prev[k - 1]
        prev = prev[:k + 1]
        if not (add.mnemonic == "add" and len(add.operands) == 2 and add.operands[0].type == X.X86_OP_REG
                and add.operands[0].reg == R and add.operands[1].type == X.X86_OP_REG): return None
        B = add.operands[1].reg
        if not (ld.mnemonic == "mov" and len(ld.operands) == 2 and ld.operands[0].type == X.X86_OP_REG and ld.operands[0].reg == R
                and ld.operands[1].type == X.X86_OP_MEM and ld.operands[1].mem.scale == 4 and ld.operands[1].mem.index): return None
        mm = ld.operands[1].mem
        regs = {B}
        # the base register may have been reloaded from a stack slot (mov B,[ebp-k]); follow stores to that slot
        for q in reversed(prev[:-2][-12:]):
            if q.mnemonic == "mov" and len(q.operands) == 2 and q.operands[0].type == X.X86_OP_REG and q.operands[0].reg == B:
                src = q.operands[1]
                if src.type == X.X86_OP_MEM and not src.mem.index and src.mem.base in (X.X86_REG_EBP, X.X86_REG_ESP):
                    for st in seen.values():
                        if st and st.mnemonic == "mov" and len(st.operands) == 2 and st.operands[0].type == X.X86_OP_MEM \
                                and st.operands[0].mem.base == src.mem.base and st.operands[0].mem.disp == src.mem.disp \
                                and not st.operands[0].mem.index and st.operands[1].type == X.X86_OP_REG:
                            regs.add(st.operands[1].reg)
                break
        bases = sorted(set(x.address for x in seen.values() if x and x.mnemonic == "pop" and x.operands
                           and x.operands[0].type == X.X86_OP_REG and x.operands[0].reg in regs))
        bases = sorted(set(bases) | {self.addr})   # some functions compute their own start address as the PIC base
        limit = None
        for q in reversed(prev[-10:]):
            if q.mnemonic == "cmp" and len(q.operands) == 2 and q.operands[1].type == X.X86_OP_IMM:
                limit = (q.operands[1].imm & 0xFFFFFFFF) + 1; break
        if limit is not None and limit > 4096: limit = None
        best = []
        for base in bases:
            cands = set()
            if mm.base == B: cands.add((base + mm.disp) & 0xFFFFFFFF)
            spec = self.pic_specific(ld, seen, B) if mm.base != B else []
            if spec:   # data flow pins down this jump's own table: use only that
                cands = set((base + d + mm.disp) & 0xFFFFFFFF for d in spec)
            for x in ([] if spec else seen.values()):
                if x and x.mnemonic == "lea" and len(x.operands) == 2 and x.operands[1].type == X.X86_OP_MEM \
                        and x.operands[1].mem.base == B and not x.operands[1].mem.index:
                    cands.add((base + x.operands[1].mem.disp + mm.disp) & 0xFFFFFFFF)
            for ta in cands:
                tbl = []
                for n in range(limit or 4096):
                    try: v = int.from_bytes(self.img.read(ta + 4 * n, 4), "little")
                    except Exception: break
                    t = (base + v) & 0xFFFFFFFF
                    if not (self.addr <= t < self.end): break
                    tbl.append(t)
                if limit is not None and len(tbl) < limit: continue   # guarded switch: the whole table must be valid
                if len(tbl) > len(best): best = tbl
        return best or None

    def descend(self):
        seen, work, out = {}, [self.addr] + list(self.lps), []
        self.tables = {}
        while work:
            a = work.pop()
            while a not in seen:
                i = self.decode(a)
                if i is None:
                    seen[a] = None; break
                seen[a] = i
                m = i.mnemonic
                if i.group(cs.CS_GRP_JUMP) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                    t = i.operands[0].imm
                    if self.addr <= t < self.end: work.append(t)
                if m == "jmp" and i.operands and i.operands[0].type == X.X86_OP_MEM:
                    op = i.operands[0]
                    if not op.mem.base and op.mem.index and op.mem.scale == 4:
                        prev = sorted(x for x in seen.values() if x and x.address < i.address)[-6:] if False else None
                        tbl, ta = [], op.mem.disp
                        prev = sorted((x for x in seen.values() if x and i.address - 40 <= x.address < i.address), key=lambda x: x.address)
                        limit = self.table_limit(len(prev), prev, op.mem.index) or 4096
                        n = 0
                        while n < limit:
                            t = int.from_bytes(self.img.read(ta, 4), "little")
                            if not (self.addr <= t < self.end): break
                            tbl.append(t); ta += 4; n += 1
                        self.tables[i.address] = (tbl, op.mem.index)
                        work.extend(tbl)
                if m == "jmp" and i.operands and i.operands[0].type == X.X86_OP_REG:
                    ptbl = self.pic_table(i, seen)
                    if ptbl:
                        self.tables[i.address] = (ptbl, None)
                        work.extend(ptbl)
                if m in ("ret", "jmp", "ud2", "hlt", "retf", "ljmp") or m.startswith("ret"):
                    break
                a = i.address + i.size
        ins = sorted((x for x in seen.values() if x), key=lambda x: x.address)
        # trim tables using the cmp guard now that we have linear context
        for k, i in enumerate(ins):
            if i.address in self.tables:
                tbl, reg = self.tables[i.address]
                lim = self.table_limit(k, ins, reg)
                if lim: self.tables[i.address] = (tbl[:lim], reg)
        self.bad_decode = [a for a, x in seen.items() if x is None]
        return ins

    def gen(self):
        for i in self.ins:
            if i.group(cs.CS_GRP_JUMP) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                t = i.operands[0].imm
                if t in self.addrs: self.labels.add(t)
            if i.mnemonic == "jmp" and i.operands[0].type != X.X86_OP_IMM:
                for t in self.jump_table(i): self.labels.add(t)
        f = self.out
        lps = [a for a in self.lps if a in self.addrs]
        for a in lps: self.labels.add(a)
        if lps:
            f.append("static __attribute__((noinline)) void B_%x(CPU *c, uint32_t eh_lp) { /* %s (body; C++ landing pads) */" % (self.addr, self.img.func_names[self.addr]))
            f.append("    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0; (void)cf; (void)zf; (void)sf; (void)of; (void)pf;")
            f.append("    switch (eh_lp) { %s default: break; }" % " ".join("case 0x%x: goto L_%x;" % (a, a) for a in lps))
            self.tr.eh_funcs += 1
        else:
            f.append("void F_%x(CPU *c) { /* %s */" % (self.addr, self.img.func_names[self.addr]))
            f.append("    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0; (void)cf; (void)zf; (void)sf; (void)of; (void)pf;")
        f.append("    TRACE_FN(0x%x);" % self.addr)
        for k, i in enumerate(self.ins):
            nx = self.ins[k + 1].address if k + 1 < len(self.ins) else None
            if not self.ends_block(i) and nx != i.address + i.size and (i.address + i.size) in self.addrs:
                self.labels.add(i.address + i.size)
        last = None
        for k, i in enumerate(self.ins):
            if last is not None and not self.ends_block(last) and i.address != last.address + last.size:
                ft = last.address + last.size
                self.e("goto L_%x;" % ft if ft in self.addrs else self.tail(ft, last.address))
            self.cur = i
            if i.address in self.labels:
                f.append("L_%x:;" % i.address)
            try:
                self.insn(i)
            except Exception as ex:
                self.e("bad_insn(c, 0x%x); /* %s %s : %s */" % (i.address, i.mnemonic, i.op_str, ex))
                self.tr.unsupported[i.mnemonic] = self.tr.unsupported.get(i.mnemonic, 0) + 1
            last = i
        if last is not None and not self.ends_block(last):
            ft = last.address + last.size
            self.e(self.tail(ft, last.address))
        for a in self.bad_decode:
            self.tr.unsupported["<undecodable>"] = self.tr.unsupported.get("<undecodable>", 0) + 1
        f.append("}")
        if lps:
            f.append("void F_%x(CPU *c) { EHReg eh_r; eh_enter(c, &eh_r, 0x%xu); volatile uint32_t eh_lp = 0;"
                     " if (_setjmp(eh_r.jb)) eh_lp = eh_r.lp; B_%x(c, eh_lp); eh_leave(&eh_r); }" % (self.addr, self.addr, self.addr))
        return "\n".join(f)

    def jump_table(self, i):
        return [t for t in self.tables.get(i.address, ([], 0))[0] if t in self.addrs]

    @staticmethod
    def ends_block(i):
        m = i.mnemonic
        return m in ("ret", "jmp", "ud2", "hlt", "retf", "ljmp") or m.startswith("ret")

    def target(self, t, frm):
        """C statement for transferring control (tail call) to address t."""
        return self.tail(t, frm)

    def tail(self, t, frm):
        if t in self.tr.hle:
            return "%s(c); C.esp += 4; return;" % self.tr.hle_name(t)
        if self.tr.lo <= t < self.tr.hi and t in self.img.func_names:
            self.tr.refs.add(t)
            return "F_%x(c); return;" % t
        if t in self.img.stubs:
            # ---- added for BioShock Infinite: stub to a function the game itself defines ----
            da = self.img.name_to_addr.get(self.img.stubs[t])
            if da is not None and self.tr.lo <= da < self.tr.hi:
                self.tr.refs.add(da)
                return "F_%x(c); return;" % da
            # ---- end additions ----
            return "%s(c); C.esp += 4; return;" % self.tr.imp(self.img.stubs[t])
        return "missing_func(c, 0x%x, 0x%x); return;" % (t, frm)

    # ---------- instructions ----------
    def insn(self, i):
        m, ops = i.mnemonic, i.operands
        nxt = i.address + i.size
        e = self.e
        if m in ("nop", "cld") or m.startswith("nop"):
            return
        if m == "mov":
            e(self.wr(ops[0], self.rd(ops[1], ops[0].size))); return
        if m == "movzx":
            e(self.wr(ops[0], "(uint32_t)" + self.rd(ops[1]))); return
        if m == "movsx":
            e(self.wr(ops[0], "(uint32_t)(int32_t)(%s)%s" % (ST[ops[1].size], self.rd(ops[1])))); return
        if m == "lea":
            e(self.wr(ops[0], self.mem_addr(ops[1]))); return
        if m == "push":
            e("PUSH((uint32_t)%s);" % self.rd(ops[0], 4) if ops[0].type == X.X86_OP_IMM
              else "PUSH(%s);" % self.rd(ops[0])); return
        if m == "pop":
            e("{ uint32_t v = POP(); %s }" % self.wr(ops[0], "v")); return
        if m == "leave":
            e("C.esp = C.ebp; C.ebp = POP();"); return
        if m == "ret":
            n = ops[0].imm if ops else 0
            e("C.esp += %d; return;" % (4 + n)); return
        if m == "cdq":
            e("C.edx = (int32_t)C.eax < 0 ? 0xFFFFFFFFu : 0;"); return
        if m == "ud2":
            e("bad_insn(c, 0x%x); return;" % i.address); return
        if m in ("add", "sub", "cmp", "and", "or", "xor", "test", "adc", "sbb"):
            return self.alu(m, ops)
        if m in ("inc", "dec", "neg", "not"):
            return self.unary(m, ops[0])
        if m in ("shl", "sal", "shr", "sar"):
            return self.shift(m, ops)
        if m in ("imul", "mul", "div", "idiv"):
            return self.muldiv(m, ops)
        if m == "call":
            return self.call(i, ops[0], nxt)
        if m == "jmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm
                e("goto L_%x;" % t if t in self.addrs else self.target(t, i.address))
            else:
                tbl = self.jump_table(i)
                v = self.rd(op, 4)
                if tbl:
                    cases = " ".join("case 0x%x: goto L_%x;" % (t, t) for t in sorted(set(tbl)))
                    e("switch (%s) { %s default: bad_jump(c, %s, 0x%x); return; }" % (v, cases, v, i.address))
                else:  # indirect tail call
                    e("{ uint32_t t = %s; call_ind(c, t); return; }" % v)
            return
        if m.startswith("j"):
            cc = self.cc(m[1:])
            t = ops[0].imm
            e("if (%s) %s" % (cc, "goto L_%x;" % t if t in self.addrs else "{ %s }" % self.target(t, i.address)))
            return
        if m.startswith("set"):
            e(self.wr(ops[0], "(%s) ? 1 : 0" % self.cc(m[3:]))); return
        if m.startswith("cmov"):
            e("if (%s) %s" % (self.cc(m[4:]), self.wr(ops[0], self.rd(ops[1])))); return
        if self.strop(i, m, ops):
            return
        if m == "repe cmpsb":
            e("while (C.ecx) { uint32_t a = rd8(C.esi), b = rd8(C.edi), r = a - b; C.esi++; C.edi++; C.ecx--;"
              " cf = a < b; of = ((a ^ b) & (a ^ r) & 0x80u) != 0; %s if (!zf) break; }" % self.szf("r", 1))
            return
        if self.extra(i, m, ops) or self.sse(m, ops) or self.x87(m, ops):
            return
        raise Unsupported(m)

    def strop(self, i, m, ops):
        mm = re.match(r"(rep |repe |repne )?(movs|stos|lods|scas|cmps)([bwd])$", m)
        if not mm: return False
        if any(o.type == X.X86_OP_REG and (self.rn(o.reg) or "").startswith("xmm") for o in ops):
            return False  # SSE movsd/cmpsd, not the string instructions
        if m == "repe cmpsb": return False
        rep, op, w = mm.group(1), mm.group(2), {"b": 1, "w": 2, "d": 4}[mm.group(3)]
        b = 8 * w
        acc = {1: "(uint8_t)C.eax", 2: "(uint16_t)C.eax", 4: "C.eax"}[w]
        if op == "movs": body = "wr%d(C.edi, rd%d(C.esi)); C.esi += %d; C.edi += %d;" % (b, b, w, w)
        elif op == "stos": body = "wr%d(C.edi, %s); C.edi += %d;" % (b, acc, w)
        elif op == "lods": body = "{ uint32_t v = rd%d(C.esi); C.esi += %d; C.eax = %s; }" % (b, w, {1: "(C.eax & 0xFFFFFF00u) | v", 2: "(C.eax & 0xFFFF0000u) | v", 4: "v"}[w])
        elif op == "scas": body = "{ uint32_t a = %s, bb = rd%d(C.edi), r = (a - bb) & %s; C.edi += %d; cf = a < bb; of = (((a ^ bb) & (a ^ r)) >> %d) & 1; %s }" % (acc, b, MASK[w], w, b - 1, self.szf("r", w))
        else: body = "{ uint32_t a = rd%d(C.esi), bb = rd%d(C.edi), r = (a - bb) & %s; C.esi += %d; C.edi += %d; cf = a < bb; of = (((a ^ bb) & (a ^ r)) >> %d) & 1; %s }" % (b, b, MASK[w], w, w, b - 1, self.szf("r", w))
        if not rep:
            self.e(body)
        elif rep == "rep ":
            if op == "movs" and w == 4: self.e("if (C.ecx) { MC(C.edi, 4 * C.ecx); memmove(M + C.edi, M + C.esi, 4ull * C.ecx); C.esi += 4 * C.ecx; C.edi += 4 * C.ecx; C.ecx = 0; }")
            elif op == "stos" and w == 1: self.e("if (C.ecx) { MC(C.edi, C.ecx); memset(M + C.edi, (uint8_t)C.eax, C.ecx); C.edi += C.ecx; C.ecx = 0; }")
            else: self.e("while (C.ecx) { %s C.ecx--; }" % body)
        else:
            stop = "if (!zf) break;" if rep == "repe " else "if (zf) break;"
            self.e("while (C.ecx) { %s C.ecx--; %s }" % (body, stop))
        return True

    def cc(self, c):
        alias = {"z": "e", "nz": "ne", "c": "b", "nae": "b", "nb": "ae", "nc": "ae", "na": "be",
                 "nbe": "a", "nge": "l", "nl": "ge", "ng": "le", "nle": "g", "pe": "p", "po": "np"}
        c = alias.get(c, c)
        if c not in CC: raise Unsupported("cc " + c)
        return CC[c]

    def alu(self, m, ops):
        d, s = ops
        n = d.size
        sign = 8 * n - 1
        M_ = MASK[n]
        if m == "xor" and d.type == X.X86_OP_REG and s.type == X.X86_OP_REG and d.reg == s.reg:
            self.e(self.wr(d, "0") + " zf = 1; sf = 0; cf = 0; of = 0; pf = 1;"); return
        a, b = self.rd(d), self.rd(s, n)
        st = ["uint32_t a = %s, b = %s, r;" % (a, b)]
        if m in ("add", "adc"):
            st.append("r = (a + b%s) & %s;" % (" + cf" if m == "adc" else "", M_))
            if m == "add": st.append("cf = r < a;")
            else: st.append("cf = (uint64_t)a + b + cf > %s;" % M_)
            st.append("of = (((a ^ r) & (b ^ r)) >> %d) & 1;" % sign)
        elif m in ("sub", "cmp", "sbb"):
            if m == "sbb":
                st.append("r = (a - b - cf) & %s; cf = (uint64_t)a < (uint64_t)b + cf;" % M_)
            else:
                st.append("r = (a - b) & %s; cf = a < b;" % M_)
            st.append("of = (((a ^ b) & (a ^ r)) >> %d) & 1;" % sign)
        else:
            op = {"and": "&", "test": "&", "or": "|", "xor": "^"}[m]
            st.append("r = a %s b; cf = 0; of = 0;" % op)
        st.append(self.szf("r", n))
        if m not in ("cmp", "test"):
            st.append(self.wr(d, "r"))
        self.e("{ " + " ".join(st) + " }")

    def unary(self, m, d):
        n = d.size; sign = 8 * n - 1; M_ = MASK[n]
        a = self.rd(d)
        if m == "not":
            self.e(self.wr(d, "~%s" % a)); return
        if m == "inc":
            body = "r = (a + 1) & %s; of = r == (1u << %d);" % (M_, sign)
        elif m == "dec":
            body = "r = (a - 1) & %s; of = a == (1u << %d);" % (M_, sign)
        else:
            body = "r = (0u - a) & %s; cf = a != 0; of = a == (1u << %d);" % (M_, sign)
        self.e("{ uint32_t a = %s, r; %s %s %s }" % (a, body, self.szf("r", n), self.wr(d, "r")))

    def shift(self, m, ops):
        d = ops[0]; n = d.size; bits = 8 * n; M_ = MASK[n]
        if len(ops) == 1: cnt = "1u"
        elif ops[1].type == X.X86_OP_IMM: cnt = "%du" % (ops[1].imm & 31)
        else: cnt = "(C.ecx & 31u)"
        a = self.rd(d)
        if m in ("shl", "sal"):
            body = "r = (uint32_t)(((uint64_t)a << cc) & %s); cf = ((uint64_t)a >> (%d - cc)) & 1; of = ((r >> %d) & 1) ^ cf;" % (M_, bits, bits - 1)
        elif m == "shr":
            body = "r = a >> cc; cf = (a >> (cc - 1)) & 1; of = (a >> %d) & 1;" % (bits - 1)
        else:
            body = "r = (uint32_t)(((int32_t)(%s)a) >> cc) & %s; cf = (((int32_t)(%s)a) >> (cc - 1)) & 1; of = 0;" % (ST[n], M_, ST[n])
        self.e("{ uint32_t cc = %s; if (cc) { uint32_t a = %s, r; %s %s %s } }" % (cnt, a, body, self.szf("r", n), self.wr(d, "r")))

    def muldiv(self, m, ops):
        e = self.e
        if m == "imul" and len(ops) >= 2:
            d = ops[0]
            a = self.rd(ops[1]) if len(ops) == 3 else self.rd(d)
            b = self.rd(ops[2], d.size) if len(ops) == 3 else self.rd(ops[1], d.size)
            if d.size == 2:
                e("{ int32_t p = (int32_t)(int16_t)%s * (int32_t)(int16_t)%s; cf = of = p != (int16_t)p; %s }" % (a, b, self.wr(d, "(uint16_t)p")))
                return
            if d.size != 4: raise Unsupported("imul16")
            e("{ int64_t p = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s; uint32_t r = (uint32_t)p; "
              "cf = of = p != (int64_t)(int32_t)r; %s %s }" % (a, b, self.szf("r", 4), self.wr(d, "r")))
            return
        s = ops[0]
        if s.size == 1:   # 8-bit: AX = AL * src; div: AL = AX / src, AH = AX % src
            v = "(uint8_t)%s" % self.rd(s)
            AX = "(C.eax & 0xFFFFu)"; setax = "C.eax = (C.eax & 0xFFFF0000u) | ((uint32_t)(%s) & 0xFFFFu);"
            if m == "mul":
                e("{ uint32_t p = (uint32_t)(uint8_t)C.eax * %s; %s cf = of = (p >> 8) != 0; }" % (v, setax % "p"))
            elif m == "imul":
                e("{ int32_t p = (int32_t)(int8_t)C.eax * (int32_t)(int8_t)%s; %s cf = of = p != (int8_t)p; }" % (v, setax % "p"))
            elif m == "div":
                e("{ uint32_t nn = %s, dd = %s; uint32_t q = nn / dd, r = nn %% dd; %s }" % (AX, v, setax % "(q & 0xFF) | ((r & 0xFF) << 8)"))
            else:
                e("{ int32_t nn = (int16_t)%s, dd = (int8_t)%s; int32_t q = nn / dd, r = nn %% dd; %s }" % (AX, v, setax % "((uint32_t)q & 0xFF) | (((uint32_t)r & 0xFF) << 8)"))
            return
        if s.size == 2:   # 16-bit: DX:AX = AX * src; div: AX = DX:AX / src, DX = remainder
            v = "(uint16_t)%s" % self.rd(s)
            lo = "C.eax = (C.eax & 0xFFFF0000u) | ((uint32_t)(%s) & 0xFFFFu);"; hi = "C.edx = (C.edx & 0xFFFF0000u) | ((uint32_t)(%s) & 0xFFFFu);"
            if m == "mul":
                e("{ uint32_t p = (uint32_t)(uint16_t)C.eax * %s; %s %s cf = of = (p >> 16) != 0; }" % (v, lo % "p", hi % "p >> 16"))
            elif m == "imul":
                e("{ int32_t p = (int32_t)(int16_t)C.eax * (int32_t)(int16_t)%s; %s %s cf = of = p != (int16_t)p; }" % (v, lo % "p", hi % "(uint32_t)p >> 16"))
            elif m == "div":
                e("{ uint32_t nn = ((C.edx & 0xFFFFu) << 16) | (C.eax & 0xFFFFu), dd = %s; uint32_t q = nn / dd, r = nn %% dd; %s %s }" % (v, lo % "q", hi % "r"))
            else:
                e("{ int32_t nn = (int32_t)(((C.edx & 0xFFFFu) << 16) | (C.eax & 0xFFFFu)), dd = (int16_t)%s; int32_t q = nn / dd, r = nn %% dd; %s %s }" % (v, lo % "q", hi % "r"))
            return
        if s.size != 4: raise Unsupported(m + "8/16")
        v = self.rd(s)
        if m == "mul":
            e("{ uint64_t p = (uint64_t)C.eax * %s; C.eax = (uint32_t)p; C.edx = (uint32_t)(p >> 32); cf = of = C.edx != 0; }" % v)
        elif m == "imul":
            e("{ int64_t p = (int64_t)(int32_t)C.eax * (int32_t)%s; C.eax = (uint32_t)p; C.edx = (uint32_t)((uint64_t)p >> 32); cf = of = p != (int32_t)C.eax; }" % v)
        elif m == "div":
            e("{ uint64_t nn = ((uint64_t)C.edx << 32) | C.eax; uint32_t dd = %s; C.eax = (uint32_t)(nn / dd); C.edx = (uint32_t)(nn %% dd); }" % v)
        else:
            e("{ int64_t nn = (int64_t)(((uint64_t)C.edx << 32) | C.eax); int32_t dd = (int32_t)%s; C.eax = (uint32_t)(int32_t)(nn / dd); C.edx = (uint32_t)(int32_t)(nn %% dd); }" % v)

    def call(self, i, op, nxt):
        e = self.e
        if op.type == X.X86_OP_IMM:
            t = op.imm
            if t in self.img.stubs:
                name = self.img.stubs[t]
                if name == "_setjmp":
                    e("{ GJmp *j = gjmp_slot(rd32(C.esp)); gjmp_save(c, j); if (setjmp(j->host)) gjmp_restore(c, j); else C.eax = 0; }")
                else:
                    # ---- added for BioShock Infinite: stub to a function the game itself defines ----
                    da = self.img.name_to_addr.get(name)
                    if da is not None and self.tr.lo <= da < self.tr.hi:
                        self.tr.refs.add(da)
                        e("PUSH(0x%xu); F_%x(c);" % (nxt, da))
                    else:
                        e("PUSH(0x%xu); %s(c); C.esp += 4;" % (nxt, self.tr.imp(name)))
                    # ---- end additions ----
                return
            if t == nxt:   # PIC idiom: call next; pop reg
                e("PUSH(0x%xu);" % nxt)
            elif t in self.tr.hle:
                e("PUSH(0x%xu); %s(c); C.esp += 4;" % (nxt, self.tr.hle_name(t)))
            elif self.tr.lo <= t < self.tr.hi and t in self.img.func_names:
                self.tr.refs.add(t)
                e("PUSH(0x%xu); F_%x(c);" % (nxt, t))
            else:
                e("PUSH(0x%xu); missing_func(c, 0x%x, 0x%x);" % (nxt, t, i.address))
            return
        e("{ uint32_t t = %s; PUSH(0x%xu); call_ind(c, t); }" % (self.rd(op, 4), nxt))

    # ---------- misc integer / atomic ----------
    def extra(self, i, m, ops):
        e = self.e
        if m in ("rol", "ror"):
            d = ops[0]
            cnt = "1u" if len(ops) == 1 else ("%du" % (ops[1].imm & 31) if ops[1].type == X.X86_OP_IMM else "(C.ecx & 31u)")
            n = 8 * d.size
            if n == 32:
                f = "rol32" if m == "rol" else "ror32"
                e("{ uint32_t cc = %s; if (cc) { uint32_t r = %s(%s, cc); cf = %s; %s } }" % (cnt, f, self.rd(d), "r & 1" if m == "rol" else "r >> 31", self.wr(d, "r")))
            else:
                sh = "(cc %% %d)" % n
                expr = ("(uint32_t)((a << k) | (a >> (%d - k)))" if m == "rol" else "(uint32_t)((a >> k) | (a << (%d - k)))") % n
                e("{ uint32_t cc = %s; uint32_t k = %s; if (k) { uint32_t a = %s; uint32_t r = %s & %s; cf = %s; %s } }" % (cnt, sh, self.rd(d), expr, MASK[d.size], "r & 1" if m == "rol" else "(r >> %d) & 1" % (n - 1), self.wr(d, "r")))
            return True
        if m == "bswap":
            e(self.wr(ops[0], "__builtin_bswap32(%s)" % self.rd(ops[0]))); return True
        if m in ("shld", "shrd"):
            d, s_, k = ops
            cnt = "%du" % (k.imm & 31) if k.type == X.X86_OP_IMM else "(C.ecx & 31u)"
            if d.size != 4: raise Unsupported(m + "16")
            if m == "shld": body = "r = (a << cc) | (b >> (32 - cc)); cf = (a >> (32 - cc)) & 1;"
            else: body = "r = (a >> cc) | (b << (32 - cc)); cf = (a >> (cc - 1)) & 1;"
            e("{ uint32_t cc = %s; if (cc) { uint32_t a = %s, b = %s, r; %s %s %s } }" % (cnt, self.rd(d), self.rd(s_), body, self.szf("r", 4), self.wr(d, "r")))
            return True
        if m == "bt":
            d, b = ops
            if d.type == X.X86_OP_MEM and b.type == X.X86_OP_REG:
                e("{ int32_t bi = (int32_t)%s; cf = (rd8(%s + (uint32_t)(bi >> 3)) >> (bi & 7)) & 1; }" % (self.rd(b), self.mem_addr(d)))
            else:
                e("cf = (%s >> (%s & %d)) & 1;" % (self.rd(d), self.rd(b, d.size), 8 * d.size - 1))
            return True
        if m in ("bsr", "bsf"):
            d, s_ = ops
            f = "31 - __builtin_clz(v)" if m == "bsr" else "__builtin_ctz(v)"
            e("{ uint32_t v = %s; zf = v == 0; if (v) %s }" % (self.rd(s_), self.wr(d, f)))
            return True
        if m == "cwde":
            e("C.eax = (uint32_t)(int32_t)(int16_t)C.eax;"); return True
        if m == "cbw":
            e("C.eax = (C.eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)C.eax;"); return True
        if m == "cwd":
            e("C.edx = (C.edx & 0xFFFF0000u) | (((int16_t)C.eax < 0) ? 0xFFFFu : 0);"); return True
        if m == "xchg":
            a, b = ops
            if b.type == X.X86_OP_MEM and a.type != X.X86_OP_MEM: a, b = b, a
            if a.type == X.X86_OP_MEM and a.size in (1, 2, 4):
                # xchg with memory is implicitly locked on x86 (spinlocks, InterlockedExchange): keep it atomic
                ty = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}[a.size]
                e("{ uint32_t t = __atomic_exchange_n((%s *)(M + %s), (%s)%s, __ATOMIC_SEQ_CST); %s }" % (ty, self.mem_addr(a), ty, self.rd(b), self.wr(b, "t")))
                return True
            e("{ uint32_t t = %s; %s %s }" % (self.rd(a), self.wr(a, self.rd(b)), self.wr(b, "t")))
            return True
        if m == "pushfd":
            e("PUSH(0x202u | cf | (pf << 2) | (zf << 6) | (sf << 7) | (of << 11));"); return True
        if m == "popfd":
            e("{ uint32_t f = POP(); cf = f & 1; pf = (f >> 2) & 1; zf = (f >> 6) & 1; sf = (f >> 7) & 1; of = (f >> 11) & 1; }"); return True
        if m in ("clc", "stc", "cmc"):
            e({"clc": "cf = 0;", "stc": "cf = 1;", "cmc": "cf ^= 1;"}[m]); return True
        if m in ("emms", "wait", "fwait", "lfence", "mfence", "sfence", "pause"):
            return True
        if m == "rdtsc":
            e("{ uint64_t t = __builtin_readcyclecounter(); C.eax = (uint32_t)t; C.edx = (uint32_t)(t >> 32); }"); return True
        if m == "cpuid":
            e("cpu_cpuid(c);"); self.tr.need_cpuid = True; return True
        if m in ("lock cmpxchg8b", "cmpxchg8b"):   # 64-bit compare-exchange: edx:eax vs [m64]; equal -> store ecx:ebx
            a_ = self.mem_addr(ops[0])
            e("{ uint64_t exp = ((uint64_t)C.edx << 32) | C.eax, des = ((uint64_t)C.ecx << 32) | C.ebx; uint32_t ga = %s;"
              " if ((ga & 7) == 0) zf = __atomic_compare_exchange_n((uint64_t *)(M + ga), &exp, des, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);"
              " else { uint64_t cur = rd64(ga); zf = cur == exp; if (zf) wr64(ga, des); else exp = cur; }"
              " if (!zf) { C.eax = (uint32_t)exp; C.edx = (uint32_t)(exp >> 32); } }" % a_)
            return True
        if m.startswith("lock "):
            op = m[5:]
            d = ops[0]
            if d.type != X.X86_OP_MEM or d.size != 4: raise Unsupported(m)
            p = "(uint32_t *)(M + %s)" % self.mem_addr(d)
            if op == "xadd":
                e("{ uint32_t b = %s; uint32_t a = __atomic_fetch_add(%s, b, __ATOMIC_SEQ_CST); uint32_t r = a + b; cf = r < a; of = (((a ^ r) & (b ^ r)) >> 31) & 1; %s %s }" % (self.rd(ops[1]), p, self.szf("r", 4), self.wr(ops[1], "a")))
            elif op in ("inc", "dec", "add", "sub"):
                b = "1u" if op in ("inc", "dec") else self.rd(ops[1], 4)
                fn = "__atomic_add_fetch" if op in ("inc", "add") else "__atomic_sub_fetch"
                e("{ uint32_t r = %s(%s, %s, __ATOMIC_SEQ_CST); %s }" % (fn, p, b, self.szf("r", 4)))
            elif op == "cmpxchg":
                e("{ uint32_t exp = C.eax; zf = __atomic_compare_exchange_n(%s, &exp, %s, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); if (!zf) C.eax = exp; }" % (p, self.rd(ops[1])))
            elif op in ("or", "and", "xor"):
                fn = {"or": "__atomic_or_fetch", "and": "__atomic_and_fetch", "xor": "__atomic_xor_fetch"}[op]
                e("{ uint32_t r = %s(%s, %s, __ATOMIC_SEQ_CST); cf = of = 0; %s }" % (fn, p, self.rd(ops[1], 4), self.szf("r", 4)))
            else:
                raise Unsupported(m)
            return True
        return False

    # ---------- SSE ----------
    def sse(self, m, ops):
        e = self.e
        def src_f(op):
            return "%s.f[0]" % self.xmm(op) if op.type == X.X86_OP_REG else "rdf(%s)" % self.mem_addr(op)
        def src_d(op):
            return "%s.d[0]" % self.xmm(op) if op.type == X.X86_OP_REG else "rdd(%s)" % self.mem_addr(op)
        def src_x(op):
            return self.xmm(op) if op.type == X.X86_OP_REG else "rdx(%s)" % self.mem_addr(op)
        d = ops[0] if ops else None
        if m in ("movss", "movsd"):
            fl, ty, w = ("f", "float", "wrf") if m == "movss" else ("d", "double", "wrd")
            s = ops[1]
            if d.type == X.X86_OP_MEM:
                e("%s(%s, %s.%s[0]);" % (w, self.mem_addr(d), self.xmm(s), fl))
            elif s.type == X.X86_OP_MEM:
                e("{ XMM x = {0}; x.%s[0] = %s; %s = x; }" % (fl, src_f(s) if fl == "f" else src_d(s), self.xmm(d)))
            else:
                e("%s.%s[0] = %s.%s[0];" % (self.xmm(d), fl, self.xmm(s), fl))
            return True
        if m in ("movaps", "movups", "movdqa", "movdqu", "movapd"):
            s = ops[1]
            if d.type == X.X86_OP_MEM: e("wrx(%s, %s);" % (self.mem_addr(d), self.xmm(s)))
            else: e("%s = %s;" % (self.xmm(d), src_x(s)))
            return True
        if m == "movd":
            s = ops[1]
            if d.type == X.X86_OP_REG and self.rn(d.reg).startswith("xmm"):
                e("{ XMM x = {0}; x.u[0] = %s; %s = x; }" % (self.rd(s, 4), self.xmm(d)))
            else:
                e(self.wr(d, "%s.u[0]" % self.xmm(s)))
            return True
        def isx(op): return op.type == X.X86_OP_REG and (self.rn(op.reg) or "").startswith("xmm")
        mc = re.match(r"cmp(eq|lt|le|unord|neq|nlt|nle|ord)(ss|sd)$", m)
        if mc:
            pred, ty = mc.groups()
            a_, b_ = ("%s.f[0]" % self.xmm(d), src_f(ops[1])) if ty == "ss" else ("%s.d[0]" % self.xmm(d), src_d(ops[1]))
            cond = {"eq": "a == b", "lt": "a < b", "le": "a <= b", "unord": "a != a || b != b",
                    "neq": "!(a == b)", "nlt": "!(a < b)", "nle": "!(a <= b)", "ord": "a == a && b == b"}[pred]
            if ty == "ss": e("{ float a = %s, b = %s; %s.u[0] = (%s) ? 0xFFFFFFFFu : 0; }" % (a_, b_, self.xmm(d), cond))
            else: e("{ double a = %s, b = %s; %s.q[0] = (%s) ? ~0ull : 0; }" % (a_, b_, self.xmm(d), cond))
            return True
        if m in ("sqrtsd",):
            e("%s.d[0] = sqrt(%s);" % (self.xmm(d), src_d(ops[1]))); return True
        if m in ("minsd", "maxsd"):
            o = "<" if m == "minsd" else ">"
            e("{ double a = %s.d[0], b = %s; %s.d[0] = (a %s b) ? a : b; }" % (self.xmm(d), src_d(ops[1]), self.xmm(d), o)); return True
        if m == "cvtdq2pd":
            e("{ XMM s = %s; %s.d[0] = (double)s.i[0]; %s.d[1] = (double)s.i[1]; }" % (src_x(ops[1]) if isx(ops[1]) else "rdx(%s)" % self.mem_addr(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m == "cvttpd2dq":
            e("{ XMM s = %s; %s.i[0] = cvtt_i32(s.d[0]); %s.i[1] = cvtt_i32(s.d[1]); %s.q[1] = 0; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d), self.xmm(d))); return True
        if m == "cvtsd2si":
            e(self.wr(d, "(uint32_t)(int32_t)lrint(%s)" % src_d(ops[1]))); return True
        if m == "cvtss2si":
            e(self.wr(d, "(uint32_t)(int32_t)lrintf(%s)" % src_f(ops[1]))); return True
        mp = re.match(r"(add|sub|mul|div)pd$", m)
        if mp:
            o = {"add": "+", "sub": "-", "mul": "*", "div": "/"}[mp.group(1)]
            e("{ XMM s = %s; %s.d[0] %s= s.d[0]; %s.d[1] %s= s.d[1]; }" % (src_x(ops[1]), self.xmm(d), o, self.xmm(d), o)); return True
        if m == "movq" and (isx(d) or isx(ops[1])):
            s_ = ops[1]
            if isx(d): e("{ XMM x = {0}; x.q[0] = %s; %s = x; }" % ("%s.q[0]" % self.xmm(s_) if isx(s_) else "rd64(%s)" % self.mem_addr(s_), self.xmm(d)))
            else: e("wr64(%s, %s.q[0]);" % (self.mem_addr(d), self.xmm(s_)))
            return True
        if m in ("movhpd", "movhps", "movlpd", "movlps"):
            k = 1 if "h" in m[3] else 0
            if d.type == X.X86_OP_MEM: e("wr64(%s, %s.q[%d]);" % (self.mem_addr(d), self.xmm(ops[1]), k))
            else: e("%s.q[%d] = rd64(%s);" % (self.xmm(d), k, self.mem_addr(ops[1])))
            return True
        if m == "punpckldq" and isx(d):
            e("{ XMM s = %s, x; x.u[0] = %s.u[0]; x.u[1] = s.u[0]; x.u[2] = %s.u[1]; x.u[3] = s.u[1]; %s = x; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d), self.xmm(d))); return True
        if m in ("psllq", "psrlq") and isx(d) and ops[1].type == X.X86_OP_IMM:
            k = ops[1].imm; o = "<<" if m == "psllq" else ">>"
            e("for (int k = 0; k < 2; k++) %s.q[k] = %s;" % (self.xmm(d), "0" if k > 63 else "%s.q[k] %s %d" % (self.xmm(d), o, k))); return True
        if m in ("psrld", "psrad") and isx(d) and ops[1].type == X.X86_OP_IMM:
            k = ops[1].imm
            ex = ("%s.u[k] >> %d" if m == "psrld" else "(uint32_t)(%s.i[k] >> %d)") % (self.xmm(d), min(k, 31))
            e("for (int k = 0; k < 4; k++) %s.u[k] = %s;" % (self.xmm(d), "0" if (k > 31 and m == "psrld") else ex)); return True
        # ---- added for BioShock Infinite: packed-float / SSE2-4.1 ops ----
        mq = re.match(r"cmp(eq|lt|le|unord|neq|nlt|nle|ord)ps$", m)
        if mq:
            cond = {"eq": "a == b", "lt": "a < b", "le": "a <= b", "unord": "a != a || b != b",
                    "neq": "!(a == b)", "nlt": "!(a < b)", "nle": "!(a <= b)", "ord": "a == a && b == b"}[mq.group(1)]
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) { float a = %s.f[k], b = s.f[k]; %s.u[k] = (%s) ? 0xFFFFFFFFu : 0; } }" % (src_x(ops[1]), self.xmm(d), self.xmm(d), cond)); return True
        if m in ("minps", "maxps"):
            o = "<" if m == "minps" else ">"
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) { float a = %s.f[k], b = s.f[k]; %s.f[k] = (a %s b) ? a : b; } }" % (src_x(ops[1]), self.xmm(d), self.xmm(d), o)); return True
        if m in ("sqrtps", "rcpps", "rsqrtps"):
            ex = {"sqrtps": "sqrtf(s.f[k])", "rcpps": "1.0f / s.f[k]", "rsqrtps": "1.0f / sqrtf(s.f[k])"}[m]
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) %s.f[k] = %s; }" % (src_x(ops[1]), self.xmm(d), ex)); return True
        if m == "rsqrtss":
            e("%s.f[0] = 1.0f / sqrtf(%s);" % (self.xmm(d), src_f(ops[1]))); return True
        if m == "shufps" and isx(d):
            i8 = ops[2].imm
            e("{ XMM s = %s, x; x.u[0] = %s.u[%d]; x.u[1] = %s.u[%d]; x.u[2] = s.u[%d]; x.u[3] = s.u[%d]; %s = x; }" % (
                src_x(ops[1]), self.xmm(d), i8 & 3, self.xmm(d), (i8 >> 2) & 3, (i8 >> 4) & 3, (i8 >> 6) & 3, self.xmm(d))); return True
        if m == "pshufd" and isx(d):
            i8 = ops[2].imm
            e("{ XMM s = %s, x; x.u[0] = s.u[%d]; x.u[1] = s.u[%d]; x.u[2] = s.u[%d]; x.u[3] = s.u[%d]; %s = x; }" % (
                src_x(ops[1]), i8 & 3, (i8 >> 2) & 3, (i8 >> 4) & 3, (i8 >> 6) & 3, self.xmm(d))); return True
        if m == "unpcklps" and isx(d):
            e("{ XMM s = %s, x; x.u[0] = %s.u[0]; x.u[1] = s.u[0]; x.u[2] = %s.u[1]; x.u[3] = s.u[1]; %s = x; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d), self.xmm(d))); return True
        if m in ("unpckhps", "punpckhdq") and isx(d):
            e("{ XMM s = %s, x; x.u[0] = %s.u[2]; x.u[1] = s.u[2]; x.u[2] = %s.u[3]; x.u[3] = s.u[3]; %s = x; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d), self.xmm(d))); return True
        if m == "movlhps":
            e("%s.q[1] = %s.q[0];" % (self.xmm(d), self.xmm(ops[1]))); return True
        if m == "movhlps":
            e("%s.q[0] = %s.q[1];" % (self.xmm(d), self.xmm(ops[1]))); return True
        if m == "pandn" and isx(d):
            e("{ XMM s = %s; for (int k = 0; k < 2; k++) %s.q[k] = ~%s.q[k] & s.q[k]; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m == "pcmpeqd" and isx(d):
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) %s.u[k] = (%s.u[k] == s.u[k]) ? 0xFFFFFFFFu : 0; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m == "paddw" and isx(d):
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) { uint32_t a = %s.u[k], b = s.u[k]; %s.u[k] = ((a & 0x7fff7fffu) + (b & 0x7fff7fffu)) ^ ((a ^ b) & 0x80008000u); } }" % (src_x(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m == "insertps":
            i8 = ops[2].imm
            cs, cd, zm = ((i8 >> 6) & 3 if isx(ops[1]) else 0), (i8 >> 4) & 3, i8 & 15
            sdef = "XMM s = %s;" % src_x(ops[1]) if isx(ops[1]) else "XMM s = {0}; s.u[0] = %s;" % self.rd(ops[1], 4)
            zs = "".join(" x.u[%d] = 0;" % k for k in range(4) if (zm >> k) & 1)
            e("{ %s XMM x = %s; x.u[%d] = s.u[%d];%s %s = x; }" % (sdef, self.xmm(d), cd, cs, zs, self.xmm(d))); return True
        if m == "extractps":
            e(self.wr(d, "%s.u[%d]" % (self.xmm(ops[1]), ops[2].imm & 3))); return True
        if m == "pinsrd" and isx(d):
            e("%s.u[%d] = %s;" % (self.xmm(d), ops[2].imm & 3, self.rd(ops[1], 4))); return True
        if m == "movmskps":
            x_ = self.xmm(ops[1])
            e(self.wr(d, "((%s.u[0] >> 31) | ((%s.u[1] >> 31) << 1) | ((%s.u[2] >> 31) << 2) | ((%s.u[3] >> 31) << 3))" % (x_, x_, x_, x_))); return True
        if m in ("roundss", "roundsd"):
            i8 = ops[2].imm
            sd_ = m == "roundsd"
            fn = {0: "rint", 1: "floor", 2: "ceil", 3: "trunc"}[0 if i8 & 4 else i8 & 3] + ("" if sd_ else "f")
            if sd_: e("%s.d[0] = %s(%s);" % (self.xmm(d), fn, src_d(ops[1])))
            else: e("%s.f[0] = %s(%s);" % (self.xmm(d), fn, src_f(ops[1])))
            return True
        if m == "haddpd":
            e("{ XMM s = %s; double a = %s.d[0] + %s.d[1], b = s.d[0] + s.d[1]; %s.d[0] = a; %s.d[1] = b; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d), self.xmm(d), self.xmm(d))); return True
        if m == "prefetcht0":
            e("/* prefetch */"); return True
        if m in ("punpcklwd", "punpckhwd") and isx(d):
            o = 4 if m == "punpckhwd" else 0
            e("{ XMM s = %s, x; uint16_t a[8], b[8], r[8]; memcpy(a, &%s, 16); memcpy(b, &s, 16); for (int k = 0; k < 4; k++) { r[2*k] = a[k+%d]; r[2*k+1] = b[k+%d]; } memcpy(&x, r, 16); %s = x; }" % (src_x(ops[1]), self.xmm(d), o, o, self.xmm(d))); return True
        if m == "punpcklbw" and isx(d):
            e("{ XMM s = %s, x; uint8_t a[16], b[16], r[16]; memcpy(a, &%s, 16); memcpy(b, &s, 16); for (int k = 0; k < 8; k++) { r[2*k] = a[k]; r[2*k+1] = b[k]; } memcpy(&x, r, 16); %s = x; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m == "cvtdq2ps" and isx(d):
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) %s.f[k] = (float)s.i[k]; }" % (src_x(ops[1]), self.xmm(d))); return True
        if m == "pextrb":
            e(self.wr(d, "(uint32_t)(((uint8_t *)&%s)[%d])" % (self.xmm(ops[1]), ops[2].imm & 15))); return True
        if m == "pextrd":
            e(self.wr(d, "%s.u[%d]" % (self.xmm(ops[1]), ops[2].imm & 3))); return True
        if m == "blendps" and isx(d):
            i8 = ops[2].imm
            e("{ XMM s = %s;%s }" % (src_x(ops[1]), "".join(" %s.u[%d] = s.u[%d];" % (self.xmm(d), k, k) for k in range(4) if (i8 >> k) & 1))); return True
        if m == "psubd" and isx(d):
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) %s.u[k] -= s.u[k]; }" % (src_x(ops[1]), self.xmm(d))); return True
        if m == "pcmpgtd" and isx(d):
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) %s.u[k] = (%s.i[k] > s.i[k]) ? 0xFFFFFFFFu : 0; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m == "pcmpgtw" and isx(d):
            e("{ XMM s = %s, x; int16_t a[8], b[8], r[8]; memcpy(a, &%s, 16); memcpy(b, &s, 16); for (int k = 0; k < 8; k++) r[k] = (a[k] > b[k]) ? -1 : 0; memcpy(&x, r, 16); %s = x; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m in ("pslld", "psrld", "psrad") and isx(d):
            n = str(ops[1].imm) if ops[1].type == X.X86_OP_IMM else "%s.q[0]" % self.xmm(ops[1])
            ex = {"pslld": "n > 31 ? 0 : %s.u[k] << n", "psrld": "n > 31 ? 0 : %s.u[k] >> n", "psrad": "(uint32_t)(%s.i[k] >> (n > 31 ? 31 : n))"}[m] % self.xmm(d)
            e("{ uint64_t n = %s; for (int k = 0; k < 4; k++) %s.u[k] = %s; }" % (n, self.xmm(d), ex)); return True
        if m == "packssdw" and isx(d):
            e("{ XMM s = %s, x; int32_t v[8]; int16_t r[8]; memcpy(v, &%s, 16); memcpy(v + 4, &s, 16); for (int k = 0; k < 8; k++) r[k] = v[k] > 32767 ? 32767 : v[k] < -32768 ? -32768 : v[k]; memcpy(&x, r, 16); %s = x; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m == "rcpss":
            e("%s.f[0] = 1.0f / %s;" % (self.xmm(d), src_f(ops[1]))); return True
        if m == "stmxcsr":
            e("wr32(%s, 0x1F80u);" % self.mem_addr(d)); return True
        if m == "ldmxcsr":
            e("/* ldmxcsr ignored */"); return True
        # MMX (64-bit mm registers) and remaining SSE2/SSSE3/SSE4.1 integer ops
        def ism(op): return op.type == X.X86_OP_REG and (self.rn(op.reg) or "").startswith("mm")
        def mmr(op): return "C.mm[%s]" % self.rn(op.reg)[2:]
        def src_m(op): return mmr(op) if ism(op) else ("%s.q[0]" % self.xmm(op) if isx(op) else "rd64(%s)" % self.mem_addr(op))
        if m == "movq" and (ism(d) or ism(ops[1])):
            if ism(d): e("%s = %s;" % (mmr(d), src_m(ops[1])))
            elif isx(d): e("{ XMM x = {0}; x.q[0] = %s; %s = x; }" % (mmr(ops[1]), self.xmm(d)))
            else: e("wr64(%s, %s);" % (self.mem_addr(d), mmr(ops[1])))
            return True
        if m == "movd" and (ism(d) or ism(ops[1])):
            if ism(d): e("%s = (uint64_t)%s;" % (mmr(d), self.rd(ops[1], 4)))
            else: e(self.wr(d, "(uint32_t)%s" % mmr(ops[1])))
            return True
        if m in ("pxor", "pand", "por", "pandn") and ism(d):
            o = {"pxor": "%s ^ s", "pand": "%s & s", "por": "%s | s", "pandn": "~%s & s"}[m] % mmr(d)
            e("{ uint64_t s = %s; %s = %s; }" % (src_m(ops[1]), mmr(d), o)); return True
        if m in ("pslld", "psrld", "psrad") and ism(d):
            n = str(ops[1].imm) if ops[1].type == X.X86_OP_IMM else src_m(ops[1])
            ex = {"pslld": "n > 31 ? 0 : v[k] << n", "psrld": "n > 31 ? 0 : v[k] >> n", "psrad": "(uint32_t)((int32_t)v[k] >> (n > 31 ? 31 : n))"}[m]
            e("{ uint64_t n = %s; uint32_t v[2]; memcpy(v, &%s, 8); for (int k = 0; k < 2; k++) v[k] = %s; memcpy(&%s, v, 8); }" % (n, mmr(d), ex, mmr(d))); return True
        if m in ("psllq", "psrlq") and ism(d):
            n = str(ops[1].imm) if ops[1].type == X.X86_OP_IMM else src_m(ops[1])
            e("{ uint64_t n = %s; %s = n > 63 ? 0 : %s %s n; }" % (n, mmr(d), mmr(d), "<<" if m == "psllq" else ">>")); return True
        if m in ("paddd", "psubd") and ism(d):
            e("{ uint64_t s = %s; uint32_t a[2], b[2]; memcpy(a, &%s, 8); memcpy(b, &s, 8); for (int k = 0; k < 2; k++) a[k] %s= b[k]; memcpy(&%s, a, 8); }" % (src_m(ops[1]), mmr(d), "+" if m == "paddd" else "-", mmr(d))); return True
        if m == "pshufw":
            i8 = ops[2].imm
            e("{ uint64_t s = %s; uint16_t a[4], r[4]; memcpy(a, &s, 8); r[0] = a[%d]; r[1] = a[%d]; r[2] = a[%d]; r[3] = a[%d]; memcpy(&%s, r, 8); }" % (src_m(ops[1]), i8 & 3, (i8 >> 2) & 3, (i8 >> 4) & 3, (i8 >> 6) & 3, mmr(d))); return True
        if m == "cvtpi2ps":
            e("{ uint64_t s = %s; int32_t v[2]; memcpy(v, &s, 8); %s.f[0] = (float)v[0]; %s.f[1] = (float)v[1]; }" % (src_m(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m == "cvtps2pi":
            e("{ XMM s = %s; int32_t v[2]; for (int k = 0; k < 2; k++) { float f = s.f[k]; v[k] = (f > -2147483649.0f && f < 2147483648.0f) ? (int32_t)lrintf(f) : INT32_MIN; } memcpy(&%s, v, 8); }" % (src_x(ops[1]) if isx(ops[1]) else "rdx(%s)" % self.mem_addr(ops[1]), mmr(d))); return True
        if m == "movupd":
            s_ = ops[1]
            if d.type == X.X86_OP_MEM: e("wrx(%s, %s);" % (self.mem_addr(d), self.xmm(s_)))
            else: e("%s = %s;" % (self.xmm(d), src_x(s_)))
            return True
        if m == "packuswb" and isx(d):
            e("{ XMM s = %s, x; int16_t v[16]; uint8_t r[16]; memcpy(v, &%s, 16); memcpy(v + 8, &s, 16); for (int k = 0; k < 16; k++) r[k] = v[k] < 0 ? 0 : v[k] > 255 ? 255 : (uint8_t)v[k]; memcpy(&x, r, 16); %s = x; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m == "packusdw" and isx(d):
            e("{ XMM s = %s, x; int32_t v[8]; uint16_t r[8]; memcpy(v, &%s, 16); memcpy(v + 4, &s, 16); for (int k = 0; k < 8; k++) r[k] = v[k] < 0 ? 0 : v[k] > 65535 ? 65535 : (uint16_t)v[k]; memcpy(&x, r, 16); %s = x; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m == "pinsrw" and isx(d):
            k = ops[2].imm & 7
            e("{ uint16_t w = (uint16_t)%s; memcpy((uint8_t *)&%s + %d, &w, 2); }" % (self.rd(ops[1], 4) if ops[1].type == X.X86_OP_REG else "rd16(%s)" % self.mem_addr(ops[1]), self.xmm(d), 2 * k)); return True
        if m == "pextrw":
            e(self.wr(d, "(uint32_t)((uint16_t *)&%s)[%d]" % (self.xmm(ops[1]) if isx(ops[1]) else mmr(ops[1]), ops[2].imm & 7))); return True
        if m in ("pslldq", "psrldq") and isx(d):
            n = ops[1].imm
            if m == "pslldq": e("{ uint8_t a[16], r[16] = {0}; memcpy(a, &%s, 16); for (int k = %d; k < 16; k++) r[k] = a[k - %d]; memcpy(&%s, r, 16); }" % (self.xmm(d), min(n, 16), min(n, 16), self.xmm(d)))
            else: e("{ uint8_t a[16], r[16] = {0}; memcpy(a, &%s, 16); for (int k = 0; k + %d < 16; k++) r[k] = a[k + %d]; memcpy(&%s, r, 16); }" % (self.xmm(d), min(n, 16), min(n, 16), self.xmm(d)))
            return True
        if m == "cvtps2dq" and isx(d):
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) { float f = s.f[k]; %s.i[k] = (f > -2147483649.0f && f < 2147483648.0f) ? (int32_t)lrintf(f) : INT32_MIN; } }" % (src_x(ops[1]), self.xmm(d))); return True
        if m == "palignr" and isx(d):
            n = ops[2].imm
            e("{ XMM s = %s; uint8_t t[32], r[16]; memcpy(t, &s, 16); memcpy(t + 16, &%s, 16); for (int k = 0; k < 16; k++) r[k] = (k + %d) < 32 ? t[k + %d] : 0; memcpy(&%s, r, 16); }" % (src_x(ops[1]), self.xmm(d), n, n, self.xmm(d))); return True
        # ---- end additions ----
        if m in ("unpcklpd", "cvtps2pd", "cvtpd2ps"):
            raise Unsupported(m)
        if m in ("xorps", "andps", "orps", "por", "pxor", "pand", "andnps", "andnpd", "xorpd", "andpd", "orpd") and not isx(d):
            raise Unsupported("mmx " + m)
        ar = {"add": "+", "sub": "-", "mul": "*", "div": "/"}
        mm = re.match(r"(add|sub|mul|div)(ss|sd|ps)$", m)
        if mm:
            o = ar[mm.group(1)]
            if mm.group(2) == "ss": e("%s.f[0] = %s.f[0] %s %s;" % (self.xmm(d), self.xmm(d), o, src_f(ops[1])))
            elif mm.group(2) == "sd": e("%s.d[0] = %s.d[0] %s %s;" % (self.xmm(d), self.xmm(d), o, src_d(ops[1])))
            else: e("{ XMM s = %s; for (int k = 0; k < 4; k++) %s.f[k] %s= s.f[k]; }" % (src_x(ops[1]), self.xmm(d), o))
            return True
        if m in ("minss", "maxss"):
            o = "<" if m == "minss" else ">"
            e("{ float a = %s.f[0], b = %s; %s.f[0] = (a %s b) ? a : b; }" % (self.xmm(d), src_f(ops[1]), self.xmm(d), o))
            return True
        if m == "sqrtss":
            e("%s.f[0] = sqrtf(%s);" % (self.xmm(d), src_f(ops[1]))); return True
        if m in ("ucomiss", "comiss", "ucomisd", "comisd"):
            a, b = ("%s.f[0]" % self.xmm(d), src_f(ops[1])) if m.endswith("ss") else ("%s.d[0]" % self.xmm(d), src_d(ops[1]))
            e("{ double a = %s, b = %s; if (a != a || b != b) { zf = pf = cf = 1; } else { zf = a == b; pf = 0; cf = a < b; } of = sf = 0; }" % (a, b))
            return True
        if m == "cmpless":
            e("%s.u[0] = (%s.f[0] <= %s) ? 0xFFFFFFFFu : 0;" % (self.xmm(d), self.xmm(d), src_f(ops[1]))); return True
        if m == "cvtss2sd":
            e("%s.d[0] = (double)%s;" % (self.xmm(d), src_f(ops[1]))); return True
        if m == "cvtsd2ss":
            e("%s.f[0] = (float)%s;" % (self.xmm(d), src_d(ops[1]))); return True
        if m in ("cvtsi2ss", "cvtsi2sd"):
            fl, ty = ("f", "float") if m.endswith("ss") else ("d", "double")
            e("%s.%s[0] = (%s)(int32_t)%s;" % (self.xmm(d), fl, ty, self.rd(ops[1], 4))); return True
        if m in ("cvttss2si", "cvttsd2si"):
            s = src_f(ops[1]) if m == "cvttss2si" else src_d(ops[1])
            e(self.wr(d, "cvtt_i32((double)%s)" % s)); return True
        if m == "cvttps2dq":
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) %s.u[k] = cvtt_i32(s.f[k]); }" % (src_x(ops[1]), self.xmm(d))); return True
        if m in ("xorps", "andps", "orps", "por", "pxor", "pand", "andnps", "andnpd", "xorpd", "andpd", "orpd"):
            if m in ("xorps", "pxor", "xorpd") and ops[1].type == X.X86_OP_REG and ops[1].reg == d.reg:
                e("memset(&%s, 0, 16);" % self.xmm(d)); return True
            o = "&" if "and" in m else "|" if "or" in m and "xor" not in m else "^"
            pre = "~" if m in ("andnps", "andnpd") else ""
            e("{ XMM s = %s; for (int k = 0; k < 2; k++) %s.q[k] = (%s%s.q[k]) %s s.q[k]; }" % (src_x(ops[1]), self.xmm(d), pre, self.xmm(d), o))
            return True
        if m == "paddd":
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) %s.u[k] += s.u[k]; }" % (src_x(ops[1]), self.xmm(d))); return True
        if m == "pslld" and ops[1].type == X.X86_OP_IMM:
            c = ops[1].imm
            e("for (int k = 0; k < 4; k++) %s.u[k] = %s;" % (self.xmm(d), "0" if c > 31 else "%s.u[k] << %d" % (self.xmm(d), c))); return True
        return False

    # ---------- x87 ----------
    def x87(self, m, ops):
        e = self.e
        if m == "fldz": e("FPUSH(0.0);"); return True
        if m == "fld1": e("FPUSH(1.0);"); return True
        if m in ("fld", "fstp", "fst"):
            op = ops[0]
            if op.type == X.X86_OP_MEM:
                a = self.mem_addr(op)
                rdm = {4: "rdf", 8: "rdd", 10: "rd80"}.get(op.size)
                wrm = {4: "wrf", 8: "wrd", 10: "wr80"}.get(op.size)
                if not rdm: raise Unsupported("x87 m%d" % (8 * op.size))
                if m == "fld": e("FPUSH(%s(%s));" % (rdm, a))
                else: e("%s(%s, (%s)ST(0));%s" % (wrm, a, "float" if op.size == 4 else "double", " FPOP();" if m == "fstp" else ""))
            else:
                k = int(self.rn(op.reg)[3:-1]) if "(" in self.rn(op.reg) else 0
                if m == "fld": e("FPUSH(ST(%d));" % k)
                else: e("ST(%d) = ST(0);%s" % (k, " FPOP();" if m == "fstp" else ""))
            return True
        if m == "fnstcw": e("wr16(%s, C.fcw);" % self.mem_addr(ops[0])); return True
        if m == "fldcw": e("C.fcw = rd16(%s);" % self.mem_addr(ops[0])); return True
        if m == "fxch":
            k = int(self.rn(ops[0].reg)[3:-1]) if ops and "(" in (self.rn(ops[0].reg) or "") else 1
            e("{ double t = ST(0); ST(0) = ST(%d); ST(%d) = t; }" % (k, k)); return True
        if m == "fchs": e("ST(0) = -ST(0);"); return True
        if m == "fabs": e("ST(0) = fabs(ST(0));"); return True
        fa = re.match(r"f(add|sub|subr|mul|div|divr)(p?)$", m)
        if fa:
            op, pop = fa.groups()
            expr = {"add": "x + y", "sub": "x - y", "subr": "y - x", "mul": "x * y", "div": "x / y", "divr": "y / x"}[op]
            if len(ops) == 1 and ops[0].type == X.X86_OP_MEM:
                rdm = {4: "rdf", 8: "rdd"}[ops[0].size]
                e("{ double x = ST(0), y = %s(%s); ST(0) = %s; }" % (rdm, self.mem_addr(ops[0]), expr))
            else:
                regs = [int(self.rn(o.reg)[3:-1]) if "(" in self.rn(o.reg) else 0 for o in ops] or [1, 0]
                if len(regs) == 1: regs = [0, regs[0]]
                dst, srci = regs
                e("{ double x = ST(%d), y = ST(%d); ST(%d) = %s;%s }" % (dst, srci, dst, expr, " FPOP();" if pop else ""))
            return True
        if m == "fild" and ops[0].type == X.X86_OP_MEM:
            e("FPUSH((double)(%s)%s);" % (ST[ops[0].size], self.rd(ops[0]))); return True
        if m == "fisttp" and ops[0].type == X.X86_OP_MEM:
            e("%s FPOP();" % self.wr(ops[0], "(%s)ST(0)" % ST[ops[0].size])); return True
        if m == "fistp" and ops[0].type == X.X86_OP_MEM:
            e("%s FPOP();" % self.wr(ops[0], "(%s)fpu_round(c, ST(0))" % ST[ops[0].size])); return True
        return False


class Translator:
    def __init__(self, img, lo, hi, hle_path=None):
        self.img, self.lo, self.hi = img, lo, hi
        self.md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
        self.md.detail = True
        self.unsupported = {}
        self.imports = set()
        self.refs = set()
        self.hle = {}
        import eh32
        ehs = {x.name: x for x in img.bin.sections}.get("__eh_frame")
        self.eh_fdes, self.eh_sites = eh32.parse(img.read, ehs.virtual_address, ehs.size) if ehs else ([], {})
        self.eh_lost = self.eh_funcs = 0
        for line in (open(hle_path) if hle_path else []):
            line = line.split("#")[0].split()
            if len(line) >= 2:
                a = int(line[0], 16)
                if "outside" in line[2:] and lo <= a < hi: continue
                self.hle[a] = line[1]

    def hle_name(self, t):
        return "H_" + cname(self.hle[t])

    def imp(self, name):
        self.imports.add(name)
        return "I_" + cname(name)

    def run(self, outdir, per_file=200):
        funcs = [a for a in self.img.func_addrs if self.lo <= a < self.hi]
        os.makedirs(outdir, exist_ok=True)
        for fn in os.listdir(outdir):
            if fn.startswith("code_") and fn.endswith(".c"): os.remove(os.path.join(outdir, fn))
        bodies = [FuncGen(self, a).gen() for a in funcs]
        # Stack accesses (esp/ebp based) are thread-private: route them to the plain accessors (rds*/wrs*);
        # everything else uses the x86-ordered (TSO) accessors from rt.h.
        stack_re = re.compile(r'\b(rd|wr)(8|16|32|64|f|d|x)\(\(uint32_t\)\(C\.e(sp|bp)\b')
        bodies = [stack_re.sub(r'\1s\2((uint32_t)(C.e\3', b) for b in bodies]
        hdr = '#include "rt.h"\n#include "funcs.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n'
        for k in range(0, len(bodies), per_file):
            with open(os.path.join(outdir, "code_%03d.c" % (k // per_file)), "w") as f:
                f.write(hdr + "\n\n".join(bodies[k:k + per_file]) + "\n")
        with open(os.path.join(outdir, "funcs.h"), "w") as f:
            f.write("#pragma once\n")
            f.write("".join("void F_%x(CPU *c);\n" % a for a in funcs))
            f.write("".join("void I_%s(CPU *c);\n" % cname(n) for n in sorted(self.imports)))
            f.write("".join("void H_%s(CPU *c);\n" % cname(n) for n in sorted(self.hle.values())))
        with open(os.path.join(outdir, "tables.c"), "w") as f:
            f.write('#include "rt.h"\n#include "tables.h"\n#include "funcs.h"\n')
            f.write("const GFunc gfuncs[] = {\n")
            f.write("".join('  {0x%x, F_%x, "%s"},\n' % (a, a, self.img.func_names[a]) for a in funcs))
            f.write("};\nconst int ngfuncs = %d;\n" % len(funcs))
            # every stub gets a shim symbol (weak default = unimplemented)
            allimp = sorted({n for n in self.img.stubs.values() if n} | {n for n in self.img.ptrs.values() if n}
                            | {n for _, n in self.img.extrel})
            for n in allimp:
                f.write('__attribute__((weak)) void I_%s(CPU *c) { unimpl_import(c, "%s"); }\n' % (cname(n), n))
            for a, n in sorted(self.hle.items()):
                f.write('__attribute__((weak)) void H_%s(CPU *c) { unimpl_import(c, "%s (HLE)"); }\n' % (cname(n), n))
            def is_data(n):
                return (n.startswith(("__ZTV", "__ZTI", "__ZTS", "_k", ".objc", "___CFConstantStringClassReference",
                                      "__DefaultRuneLocale", "___stack_chk_guard", "___stdinp", "___stdoutp", "___stderrp",
                                      "_NSApp", "_NSDefaultRunLoopMode")) or "staticMetaObject" in n)
            f.write("const GImport gimports[] = {\n")
            f.write("".join('  {"%s", %s},\n' % (n, "0" if is_data(n) else "I_" + cname(n)) for n in allimp))
            f.write("};\nconst int ngimports = %d;\n" % len(allimp))
            # ---- added for BioShock Infinite: defined data symbols referenced via symbol pointers ----
            tlo = self.img.text.virtual_address; thi = tlo + self.img.text.size
            defsyms = {}
            for sy in self.img.bin.symbols:
                if getattr(sy, "numberof_sections", 0) > 0 and sy.value and (not (tlo <= sy.value < thi) or sy.value in self.img.func_names):
                    defsyms.setdefault(sy.name, sy.value)
            gdefs = [(n, defsyms[n]) for n in allimp if n in defsyms]
            f.write("const GDef gdefs[] = {\n")
            f.write("".join('  {"%s", 0x%x},\n' % (n, v) for n, v in gdefs))
            f.write("{0, 0}};\nconst int ngdefs = %d;\n" % len(gdefs))
            # ---- end additions ----
            f.write("const GStub gstubs[] = {\n")
            f.write("".join('  {0x%x, I_%s, "%s"},\n' % (a, cname(n), n) for a, n in sorted(self.img.stubs.items()) if n))
            f.write("};\nconst int ngstubs = %d;\n" % sum(1 for n in self.img.stubs.values() if n))
            f.write("const GPtr gptrs[] = {\n")
            f.write("".join('  {0x%x, "%s"},\n' % (a, n) for a, n in sorted(self.img.ptrs.items()) if n))
            f.write("};\nconst int ngptrs = %d;\n" % sum(1 for n in self.img.ptrs.values() if n))
            sec = {x.name: x for x in self.img.bin.sections}["__mod_init_func"]
            inits = [int.from_bytes(self.img.read(sec.virtual_address + 4 * k, 4), "little") for k in range(sec.size // 4)]
            f.write("const uint32_t gmodinit[] = {%s};\nconst int ngmodinit = %d;\n" % (", ".join("0x%x" % v for v in inits), len(inits)))
            cfs = {x.name: x for x in self.img.bin.sections}.get("__cfstring")
            f.write("const uint32_t gcfstr_lo = 0x%x, gcfstr_hi = 0x%x;\n" % ((cfs.virtual_address, cfs.virtual_address + cfs.size) if cfs else (0, 0)))
            f.write("const GPtr gextrel[] = {\n")
            f.write("".join('  {0x%x, "%s"},\n' % (a, n) for a, n in self.img.extrel))
            f.write("};\nconst int ngextrel = %d;\n" % len(self.img.extrel))
            f.write("const GSeg gsegs[] = {\n")
            f.write("".join('  {"%s", 0x%x, 0x%x, 0x%x, 0x%x},\n' % s for s in self.img.segments))
            f.write("};\nconst int ngsegs = %d;\n" % len(self.img.segments))
            # ---- C++ exceptions: FDE ranges, call sites with landing pads, catch actions ----
            f.write("const GFde gfdes[] = {\n")
            f.write("".join("  {0x%x, 0x%x},\n" % r for r in self.eh_fdes))
            f.write("};\nconst int ngfdes = %d;\n" % len(self.eh_fdes))
            sites, acts = [], []
            for fn, lst in self.eh_sites.items():
                for lo, hi, lp, al in lst:
                    sites.append((lo, hi, lp, fn, len(acts), len(al))); acts.extend(al)
            sites.sort()
            f.write("const GEHSite gehsites[] = {\n")
            f.write("".join("  {0x%x, 0x%x, 0x%x, 0x%x, %d, %d},\n" % x for x in sites))
            f.write("};\nconst int ngehsites = %d;\n" % len(sites))
            f.write("const GEHAct gehacts[] = {\n")
            f.write("".join("  {%d, 0x%x, %d},\n" % (fl, ti, 1 if ind else 0) for fl, ti, ind in acts) or "  {0, 0, 0},\n")
            f.write("};\nconst int ngehacts = %d;\n" % len(acts))
        known = set(funcs)
        miss = sum(1 for a in self.eh_sites if a not in known and self.lo <= a < self.hi)
        print("C++ EH: %d functions with landing pads wrapped, %d FDEs with landing pads not matching a function start, %d landing pads outside their function"
              % (self.eh_funcs, miss, self.eh_lost))
        print("functions: %d  imports used: %d" % (len(funcs), len(self.imports)))
        if self.unsupported:
            print("UNSUPPORTED:", self.unsupported)


if __name__ == "__main__":
    # recomp32.py BINARY LO HI OUTDIR [--hle FILE]
    #   BINARY  the original 32-bit Intel Mach-O executable
    #   LO HI   the address range (hex) to translate, normally the whole __text section
    #   --hle   guest functions replaced by native ones: "address name [outside]" per line; the runtime or the game
    #           project defines H_<name>. "outside" = only when that address lies outside LO..HI (partial builds)
    args = sys.argv[1:]
    hle = None
    if "--hle" in args:
        k = args.index("--hle"); hle = args[k + 1]; del args[k:k + 2]
    path, lo, hi, out = args[0], int(args[1], 16), int(args[2], 16), args[3]
    Translator(Image(path), lo, hi, hle).run(out)
