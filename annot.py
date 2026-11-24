import sys,re
m={}
for l in open('gen2/tables.c'):
    for a,n in re.findall(r'\{0x([0-9a-f]+), \w+, "([^"]+)"\}',l): m[int(a,16)]=n
for l in sys.stdin:
    t=l.split(); s=l.rstrip()
    if len(t)>=3 and t[1] in('call','jmp') and t[2].startswith('0x'): s+='   '+m.get(int(t[2],16),'')
    print(s)
