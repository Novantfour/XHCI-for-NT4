import struct,sys
d=open(sys.argv[1],'rb').read()
# WAV-Kopf
i=12; fmt=None; data=None
while i<len(d)-8:
    cid=d[i:i+4]; n=struct.unpack_from('<I',d,i+4)[0]
    if cid==b'fmt ': fmt=struct.unpack_from('<HHIIHH',d,i+8)
    if cid==b'data': data=d[i+8:i+8+n] if n and n<len(d) else d[i+8:]; break
    i+=8+n
ch,rate,bits=fmt[1],fmt[2],fmt[5]
s=struct.unpack('<%dh'%(len(data)//2),data[:len(data)//2*2])
L=s[0::ch]
# Bereich mit Ton finden
laut=[k for k,v in enumerate(L) if abs(v)>500]
if not laut: print('kein Ton'); sys.exit()
a,b=laut[0],laut[-1]
print('Datei: %d Hz, %d Kanaele; Ton von %.3f s bis %.3f s (%.3f s)'%(rate,ch,a/rate,b/rate,(b-a)/rate))
# Luecken: Stellen im Tonbereich, an denen >=8 Samples hintereinander ~0 sind
luecken=[]; k=a; 
while k<b:
    if abs(L[k])<60:
        j=k
        while j<b and abs(L[j])<60: j+=1
        if j-k>=8: luecken.append((k,j-k))
        k=j
    else: k+=1
tot=sum(n for _,n in luecken)
print('Luecken: %d, zusammen %.1f ms (%.1f %% der Tondauer)'%(len(luecken),tot*1000/rate,100*tot/(b-a)))
if luecken:
    abst=[luecken[i+1][0]-luecken[i][0] for i in range(len(luecken)-1)]
    lens=sorted(n*1000/rate for _,n in luecken)
    print('  Laenge je Luecke: min %.1f ms, Median %.1f ms, max %.1f ms'%(lens[0],lens[len(lens)//2],lens[-1]))
    if abst: print('  Abstand: Median %.1f ms'%(sorted(abst)[len(abst)//2]*1000/rate))
