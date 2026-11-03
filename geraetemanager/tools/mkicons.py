#!/usr/bin/env python3
"""Symbole fuer den Geraetemanager -- klassisches ICO (BMP, kein PNG: das
kann NT 4.0 nicht lesen), je Groesse 256 und 16 Farben, mit UND-Maske."""
import struct, sys
from PIL import Image, ImageDraw

def neu(n): return Image.new('RGBA', (n, n), (0, 0, 0, 0))

# ---------- Zeichnungen (auf 4x gezeichnet, dann verkleinert) ----------
def monitor(d, s, bild=True):
    k = s / 32.0
    R = lambda x0,y0,x1,y1: [x0*k, y0*k, x1*k, y1*k]
    d.rounded_rectangle(R(2,3,30,23), radius=2*k, fill=(70,70,78), outline=(20,20,25), width=max(1,int(k)))
    d.rectangle(R(4.5,5.5,27.5,20.5), fill=(28,78,160))
    d.rectangle(R(4.5,5.5,27.5,9), fill=(58,118,205))
    d.polygon([(13*k,23*k),(19*k,23*k),(21*k,27*k),(11*k,27*k)], fill=(95,95,105))
    d.rounded_rectangle(R(7,26.5,25,29.5), radius=1*k, fill=(120,120,130), outline=(40,40,45), width=max(1,int(k*0.7)))
    if bild:   # Geraetebaum auf dem Bildschirm
        w=(240,240,250); L=max(1,int(1.1*k))
        d.rectangle(R(7,7.5,11,10.5), fill=w)
        d.line([(9*k,10.5*k),(9*k,18*k)], fill=w, width=L)
        for y in (13,17):
            d.line([(9*k,y*k),(13*k,y*k)], fill=w, width=L)
            d.rectangle(R(13.5,y-1.5,17.5,y+1.5), fill=(250,210,70))
            d.line([(18.5*k,y*k),(24*k,y*k)], fill=(200,215,240), width=L)

def usbzeichen(d, s, cx, cy, r, grund=(34,150,60)):
    k=s/32.0; L=max(1,int(1.3*k)); w=(255,255,255)
    d.ellipse([ (cx-r)*k,(cy-r)*k,(cx+r)*k,(cy+r)*k ], fill=grund, outline=(10,70,25), width=max(1,int(k*0.8)))
    # Dreizack
    d.line([(cx*k,(cy+r*0.62)*k),(cx*k,(cy-r*0.55)*k)], fill=w, width=L)
    d.polygon([(cx*k,(cy-r*0.8)*k),((cx-r*0.22)*k,(cy-r*0.45)*k),((cx+r*0.22)*k,(cy-r*0.45)*k)], fill=w)
    d.line([(cx*k,(cy+r*0.25)*k),((cx-r*0.45)*k,(cy-r*0.05)*k),((cx-r*0.45)*k,(cy-r*0.25)*k)], fill=w, width=L)
    d.ellipse([ (cx-r*0.58)*k,(cy-r*0.42)*k,(cx-r*0.32)*k,(cy-r*0.16)*k ], fill=w)
    d.line([(cx*k,(cy+r*0.05)*k),((cx+r*0.45)*k,(cy-r*0.2)*k),((cx+r*0.45)*k,(cy-r*0.35)*k)], fill=w, width=L)
    d.rectangle([ (cx+r*0.33)*k,(cy-r*0.52)*k,(cx+r*0.57)*k,(cy-r*0.3)*k ], fill=w)
    d.ellipse([ (cx-r*0.17)*k,(cy+r*0.5)*k,(cx+r*0.17)*k,(cy+r*0.84)*k ], fill=w)

def app(s):
    im=neu(s); d=ImageDraw.Draw(im); monitor(d,s); usbzeichen(d,s,24.5,23.5,7.3); return im

def rechner(s):
    im=neu(s); d=ImageDraw.Draw(im); monitor(d,s,bild=False); return im

def klasse(s):   # Ordner mit Karte
    im=neu(s); d=ImageDraw.Draw(im); k=s/32.0
    d.polygon([(2*k,8*k),(12*k,8*k),(15*k,11*k),(30*k,11*k),(30*k,27*k),(2*k,27*k)], fill=(232,190,60), outline=(120,90,10))
    d.rectangle([3*k,13*k,29*k,26*k], fill=(250,215,95), outline=(150,110,20))
    return im

def geraet(s):   # Steckkarte
    im=neu(s); d=ImageDraw.Draw(im); k=s/32.0
    d.rectangle([3*k,7*k,29*k,22*k], fill=(40,130,70), outline=(15,60,30), width=max(1,int(k)))
    d.rectangle([9*k,10*k,19*k,18*k], fill=(35,35,40))
    d.rectangle([21*k,10*k,26*k,13*k], fill=(200,200,210))
    for x in range(5,28,3): d.rectangle([x*k,22*k,(x+1.6)*k,26*k], fill=(225,180,40))
    return im

def warnung(s):
    im=neu(s); d=ImageDraw.Draw(im); k=s/32.0
    d.polygon([(16*k,3*k),(30*k,28*k),(2*k,28*k)], fill=(250,205,30), outline=(90,60,0))
    d.rectangle([14.6*k,11*k,17.4*k,21*k], fill=(20,20,20)); d.ellipse([14.4*k,22.5*k,17.6*k,25.7*k], fill=(20,20,20))
    return im

def usbgeraet(s):
    im=neu(s); d=ImageDraw.Draw(im); usbzeichen(d,s,16,16,14.5,grund=(30,110,200)); return im

def usbhub(s):
    im=neu(s); d=ImageDraw.Draw(im); k=s/32.0
    d.rounded_rectangle([2*k,9*k,30*k,24*k], radius=3*k, fill=(60,65,80), outline=(15,15,20), width=max(1,int(k)))
    for x in (5,12,19):
        d.rectangle([x*k,14*k,(x+5)*k,19*k], fill=(20,20,25)); d.rectangle([(x+1)*k,15*k,(x+4)*k,16.5*k], fill=(40,120,220))
    d.ellipse([25*k,15*k,28*k,18*k], fill=(60,230,90))
    return im

# ---------- ICO schreiben: BMP-Eintraege, 8 und 4 Bit ----------
VGA16=[(0,0,0),(128,0,0),(0,128,0),(128,128,0),(0,0,128),(128,0,128),(0,128,128),(192,192,192),
       (128,128,128),(255,0,0),(0,255,0),(255,255,0),(0,0,255),(255,0,255),(0,255,255),(255,255,255)]

def eintrag(img, bits):
    n=img.size[0]; rgb=Image.new('RGB',(n,n),(0,0,0)); rgb.paste(img,mask=img.split()[3])
    if bits==4:
        pal=Image.new('P',(1,1)); pal.putpalette(sum((list(c) for c in VGA16),[])+[0]*(768-48))
        q=rgb.quantize(palette=pal, dither=Image.Dither.NONE); farben=VGA16
    else:
        q=rgb.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        p=(q.getpalette()+[0]*768)[:768]; farben=[tuple(p[i*3:i*3+3]) for i in range(256)]
    px=q.load(); a=img.split()[3].load()
    zeile_x=((n*bits+31)//32)*4; zeile_m=((n+31)//32)*4
    xor=b''; und=b''
    for y in range(n-1,-1,-1):
        z=bytearray(zeile_x)
        for x in range(n):
            v=px[x,y] if a[x,y]>=128 else 0
            if bits==8: z[x]=v
            else: z[x//2] |= (v<<4) if x%2==0 else v
        xor+=bytes(z)
        m=bytearray(zeile_m)
        for x in range(n):
            if a[x,y]<128: m[x//8] |= 0x80>>(x%8)
        und+=bytes(m)
    nfarben=16 if bits==4 else 256
    kopf=struct.pack('<IiiHHIIiiII',40,n,2*n,1,bits,0,len(xor)+len(und),0,0,nfarben,0)
    pal=b''.join(struct.pack('BBBB',c[2],c[1],c[0],0) for c in (farben+[(0,0,0)]*256)[:nfarben])
    return (n, nfarben if nfarben<256 else 0, bits, kopf+pal+xor+und)

def ico(pfad, zeichner, groessen):
    ein=[]
    for n in groessen:
        gross=zeichner(n*4).resize((n,n), Image.LANCZOS)
        for bits in (8,4): ein.append(eintrag(gross,bits))
    kopf=struct.pack('<HHH',0,1,len(ein)); off=6+16*len(ein); tab=b''; daten=b''
    for n,nf,bits,d in ein:
        tab+=struct.pack('<BBBBHHII',n,n,nf,0,1,bits,len(d),off+len(daten)); daten+=d
    open(pfad,'wb').write(kopf+tab+daten)

ziel=sys.argv[1]
ico(ziel+'/devmgr.ico', app, (32,16))
for name,f in (('rechner',rechner),('klasse',klasse),('geraet',geraet),('warnung',warnung),('usb',usbgeraet),('hub',usbhub)):
    ico(ziel+'/%s.ico'%name, f, (16,32))
# Vorschau fuer das Auge
v=Image.new('RGBA',(8*40,80),(212,208,200,255))
for i,(f,n) in enumerate(((app,32),(app,16),(rechner,16),(klasse,16),(geraet,16),(warnung,16),(usbgeraet,16),(usbhub,16))):
    b=f(n*4).resize((n,n),Image.LANCZOS).resize((n*2,n*2),Image.NEAREST); v.paste(b,(i*40+4,8),b)
v.save(ziel+'/vorschau.png')
