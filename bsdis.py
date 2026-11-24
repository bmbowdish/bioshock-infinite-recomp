import lief,capstone,sys
b=lief.parse("orig_copy.bin")
if hasattr(b,'at'): b=b.at(0)
syms={s.value:s.name for s in b.symbols if s.value}
# stub names: map __symbol_stub entries via la_symbol_ptr/indirect symbols
stub={}
try:
    for s in b.symbols:
        pass
    for s in b.indirect_symbols if hasattr(b,'indirect_symbols') else []:
        pass
except Exception: pass
rd=lambda a,n:bytes(b.get_content_from_virtual_address(a,n))
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
def dis(a,n=40):
    for i in md.disasm(rd(a,n*7),a):
        t=i.op_str
        if i.mnemonic in("call","jmp") and t.startswith("0x"):
            t+="  ; "+syms.get(int(t,16),"")[:70]
        print(hex(i.address),i.mnemonic,t)
        n-=1
        if n<=0: break
a=int(sys.argv[1],16); n=int(sys.argv[2]); dis(a,n)
