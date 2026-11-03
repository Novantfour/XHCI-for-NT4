import re,sys,os,collections
CALL=re.compile(r'\b([A-Za-z_]\w*)\s*\(')
DEFN=re.compile(r'^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(',re.M)
MAC=re.compile(r'^\s*#\s*define\s+(\w+)',re.M)
KW=set('if for while switch return sizeof defined do else case typedef struct union enum static inline void const volatile __attribute__ asm typeof'.split())
def defs(p):
    d=set()
    for r,_,fs in os.walk(p):
        for f in fs:
            if f.endswith(('.c','.h')):
                t=open(os.path.join(r,f),errors='ignore').read()
                d|=set(DEFN.findall(t))|set(MAC.findall(t))
    return d
echt,quell=sys.argv[1],sys.argv[2]
files=sys.argv[3:] or [f for f in os.listdir(quell) if f.endswith('.c')]
known=defs(echt)|defs(quell)|KW
c=collections.Counter()
for f in files:
    t=open(os.path.join(quell,f),errors='ignore').read()
    t=re.sub(r'/\*.*?\*/',' ',t,flags=re.S); t=re.sub(r'//[^\n]*',' ',t)
    t=re.sub(r'"(\\.|[^"\\])*"',' ',t)
    for m in CALL.findall(t):
        if m not in known: c[m]+=1
print(f"offene Aufrufe/Makros: {len(c)}")
for k,v in sorted(c.items(),key=lambda x:-x[1]): print(f"   {k:<36}{v}")
