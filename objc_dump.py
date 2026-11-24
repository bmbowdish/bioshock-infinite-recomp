import lief,struct
b=lief.parse("orig_copy.bin")
if hasattr(b,'at'): b=b.at(0)
def rd(a,n): return bytes(b.get_content_from_virtual_address(a,n))
def u32(a): return struct.unpack("<I",rd(a,4))[0]
def cstr(a):
    if not a: return ""
    s=b""
    while True:
        c=rd(a+len(s),1)
        if c==b"\0" or len(s)>200: break
        s+=c
    return s.decode("latin1")
def meths(lst,pre):
    if not lst: return
    n=u32(lst+4)
    for i in range(n):
        m=lst+8+i*12
        print(f"  {pre}{cstr(u32(m))}  imp={u32(m+8):x}")
sec=[s for s in b.sections if s.name=="__class"][0]
for i in range(sec.size//48):
    c=sec.virtual_address+i*48
    name=cstr(u32(c+8)); sup=cstr(u32(c+4))
    print("CLASS",name,":",sup)
    ml=u32(c+28)
    if ml and ml!=0xffffffff:
        # methodLists points to a list ptr or array; try as direct method list first
        first=u32(ml)
        # old ABI: methodLists -> array of pointers (terminated by -1) OR direct list
        if first<0x1000000 and first!=0 and u32(ml+4)<0x4000 and u32(ml+4)>0 and u32(ml+8)>0x1000: meths(ml,"- ")
        else: meths(first,"- ")
    # class methods via metaclass
    meta=u32(c) 
    if meta:
        ml2=u32(meta+28)
        if ml2 and ml2!=0xffffffff:
            f=u32(ml2); meths(f if f>0x1000 and u32(ml2+4)>0x4000 else ml2,"+ ")

print("=== raw instance methods of wrapper classes ===")
for i in range(sec.size//48):
    c=sec.virtual_address+i*48
    name=cstr(u32(c+8))
    if name not in("ASLAppDelegate","WrappedNSView","Bioshock3GameGuide"): continue
    ml=u32(c+28); print("CLASS",name,"methodLists",hex(ml))
    if not ml or ml==0xffffffff: continue
    # try direct list, then pointer-to-list
    for cand in (ml,u32(ml)):
        try:
            n=u32(cand+4)
            if 0<n<500 and u32(cand+8)>0x1000:
                for k in range(n):
                    m=cand+8+k*12; print("  -",cstr(u32(m)),hex(u32(m+8)))
                break
        except Exception as e: pass
