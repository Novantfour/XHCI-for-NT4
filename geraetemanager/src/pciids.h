/*++

    pciids.h -- Hersteller- und Geraetenamen aus einer PCI.IDS

    Kopflastiges Modul: alles static, nur windows.h vorausgesetzt,
    keine C-Laufzeit.  In devmgr.c einmal einbinden.

    Die Datei PCI.IDS ist die uebliche Textdatenbank von
    pci-ids.ucw.cz.  Sie wird gesucht

        1. neben der ausfuehrenden Datei
        2. in system32
        3. im Windows-Verzeichnis

    Wird sie nicht gefunden, liefern alle Abfragen NULL und der
    Aufrufer bleibt bei seiner eingebauten Tabelle.

    Aufbau der Datei:

        8086  Intel Corporation
        \t265c  82801FB/FBM USB2 EHCI Controller
        \t\t1028 0177  Dell Optiplex GX280
        C 03  Display controller
        \t00  VGA compatible controller

    Vorgehen: Datei einmal ganz einlesen, Zeilenenden durch Null
    ersetzen, UTF-8 auf Latin-1 herunterrechnen, dann einen Index
    ueber die Herstellerzeilen und die Klassenzeilen anlegen.  Alle
    zurueckgegebenen Zeiger zeigen in diesen Puffer und bleiben
    gueltig, solange das Programm laeuft -- wichtig, weil der Baum
    Klassennamen ueber Aufrufgrenzen hinweg vergleicht.

--*/

#ifndef _PCIIDS_H_
#define _PCIIDS_H_

#define PI_MAXVEN   6144
#define PI_MAXCLS    256
#define PI_MAXFILE  (12*1024*1024)
#define PI_NOOFF    0xFFFFFFFF

typedef struct _PI_IDX { DWORD Id; DWORD Off; } PI_IDX;

static BYTE*   gPiBuf   = NULL;
static DWORD   gPiLen   = 0;
static PI_IDX* gPiVen   = NULL;
static DWORD   gPiVenN  = 0;
static PI_IDX* gPiCls   = NULL;
static DWORD   gPiClsN  = 0;
static char    gPiPath[MAX_PATH+32];

/* ---------------- kleine Helfer ---------------- */

static int pi_len(const char* s){ int n=0; while(s[n]) n++; return n; }

static void pi_cpy(char* d, const char* s){ int i=0; while(s[i]){ d[i]=s[i]; i++; } d[i]=0; }

static void pi_cat(char* d, const char* s){ int i=pi_len(d), j=0; while(s[j]) d[i++]=s[j++]; d[i]=0; }

static int pi_hex(int c)
{
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}

/* genau n Hexziffern, danach darf keine weitere folgen */
static BOOL pi_hexn(const BYTE* p, int n, DWORD* out)
{
    DWORD v=0; int i, d;
    for (i=0;i<n;i++){
        d = pi_hex(p[i]);
        if (d < 0) return FALSE;
        v = (v<<4) | (DWORD)d;
    }
    if (pi_hex(p[n]) >= 0) return FALSE;
    *out = v;
    return TRUE;
}

/* Name beginnt nach dem ersten Doppelleerzeichen der Zeile */
static const char* pi_name(DWORD off)
{
    const BYTE* p = gPiBuf + off;
    while (p[0] && !(p[0]==' ' && p[1]==' ')) p++;
    if (!p[0]) return NULL;
    while (*p==' ' || *p=='\t') p++;
    return (*p) ? (const char*)p : NULL;
}

static DWORD pi_next(DWORD off)
{
    while (off < gPiLen && gPiBuf[off]) off++;
    return off + 1;
}

/* ---------------- einlesen ---------------- */

static BOOL pi_read(const char* path)
{
    HANDLE h;
    DWORD  size, got = 0;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    size = GetFileSize(h, NULL);
    if (size == 0xFFFFFFFF || size < 64 || size > PI_MAXFILE){
        CloseHandle(h); return FALSE; }

    gPiBuf = (BYTE*)VirtualAlloc(NULL, size+8, MEM_COMMIT, PAGE_READWRITE);
    if (!gPiBuf){ CloseHandle(h); return FALSE; }

    if (!ReadFile(h, gPiBuf, size, &got, NULL) || got < 64){
        VirtualFree(gPiBuf, 0, MEM_RELEASE);
        gPiBuf = NULL;
        CloseHandle(h);
        return FALSE;
    }
    CloseHandle(h);

    gPiLen = got;
    pi_cpy(gPiPath, path);
    return TRUE;
}

/*
    Zeilenenden zu Null, UTF-8 zu Latin-1.  Die Ausgabe ist nie
    laenger als die Eingabe, also geht das an Ort und Stelle.
*/
static void pi_clean(void)
{
    DWORD s=0, d=0;

    while (s < gPiLen){
        BYTE c = gPiBuf[s];

        if (c == '\r'){ s++; continue; }
        if (c == '\n'){ gPiBuf[d++] = 0; s++; continue; }
        if (c <  0x80){ gPiBuf[d++] = c; s++; continue; }

        if ((c & 0xE0) == 0xC0 && s+1 < gPiLen){
            DWORD cp = ((DWORD)(c & 0x1F) << 6) | (DWORD)(gPiBuf[s+1] & 0x3F);
            gPiBuf[d++] = (BYTE)((cp && cp < 0x100) ? cp : '?');
            s += 2;
            continue;
        }
        if ((c & 0xF0) == 0xE0){ gPiBuf[d++]='?'; s += (s+3<=gPiLen)?3:(gPiLen-s); continue; }
        if ((c & 0xF8) == 0xF0){ gPiBuf[d++]='?'; s += (s+4<=gPiLen)?4:(gPiLen-s); continue; }
        gPiBuf[d++]='?'; s++;
    }

    gPiBuf[d]   = 0;
    gPiBuf[d+1] = 0;
    gPiBuf[d+2] = 0;
    gPiLen = d;
}

static void pi_index(void)
{
    DWORD off = 0, id;

    gPiVenN = 0;
    gPiClsN = 0;

    while (off < gPiLen){
        const BYTE* p = gPiBuf + off;

        if (p[0]=='#' || p[0]==0 || p[0]=='\t' || p[0]==' '){
            off = pi_next(off);
            continue;
        }

        if (p[0]=='C' && p[1]==' '){
            if (pi_hexn(p+2, 2, &id) && gPiClsN < PI_MAXCLS){
                gPiCls[gPiClsN].Id  = id;
                gPiCls[gPiClsN].Off = off;
                gPiClsN++;
            }
        } else {
            if (pi_hexn(p, 4, &id) && gPiVenN < PI_MAXVEN){
                gPiVen[gPiVenN].Id  = id;
                gPiVen[gPiVenN].Off = off;
                gPiVenN++;
            }
        }
        off = pi_next(off);
    }
}

static BOOL pi_try(const char* dir)
{
    char p[MAX_PATH+32];
    int  n;

    pi_cpy(p, dir);
    n = pi_len(p);
    if (n && p[n-1] != '\\') pi_cat(p, "\\");
    pi_cat(p, "PCI.IDS");
    return pi_read(p);
}

static void PciIdsInit(void)
{
    char dir[MAX_PATH+32];
    DWORD n;
    int i;

    if (gPiBuf) return;

    /*  Indextabellen nur einmal belegen -- PciIdsNeuLaden ruft hier
        erneut herein.  */
    if (!gPiVen) gPiVen = (PI_IDX*)VirtualAlloc(NULL, PI_MAXVEN*sizeof(PI_IDX),
                                   MEM_COMMIT, PAGE_READWRITE);
    if (!gPiCls) gPiCls = (PI_IDX*)VirtualAlloc(NULL, PI_MAXCLS*sizeof(PI_IDX),
                                   MEM_COMMIT, PAGE_READWRITE);
    if (!gPiVen || !gPiCls) return;

    /* 1. neben der ausfuehrenden Datei */
    n = GetModuleFileNameA(NULL, dir, MAX_PATH);
    if (n && n < MAX_PATH){
        for (i=(int)n-1; i>=0; i--) if (dir[i]=='\\'){ dir[i]=0; break; }
        if (i > 0 && pi_try(dir)) goto gefunden;
    }

    /* 2. system32 */
    n = GetSystemDirectoryA(dir, MAX_PATH);
    if (n && n < MAX_PATH && pi_try(dir)) goto gefunden;

    /* 3. Windows-Verzeichnis */
    n = GetWindowsDirectoryA(dir, MAX_PATH);
    if (n && n < MAX_PATH && pi_try(dir)) goto gefunden;

    return;

gefunden:
    pi_clean();
    pi_index();

    if (gPiVenN == 0){                 /* keine brauchbare Datei */
        VirtualFree(gPiBuf, 0, MEM_RELEASE);
        gPiBuf = NULL;
        gPiLen = 0;
        gPiPath[0] = 0;
    }
}

/* ---------------- Abfragen ---------------- */

static DWORD pi_venoff(WORD ven)
{
    DWORD i;
    if (!gPiBuf) return PI_NOOFF;
    for (i=0;i<gPiVenN;i++)
        if (gPiVen[i].Id == (DWORD)ven) return gPiVen[i].Off;
    return PI_NOOFF;
}

static DWORD pi_devoff(WORD ven, WORD dev)
{
    DWORD off = pi_venoff(ven), id;

    if (off == PI_NOOFF) return PI_NOOFF;
    off = pi_next(off);

    while (off < gPiLen){
        const BYTE* p = gPiBuf + off;
        if (p[0] == 0 || p[0] == '#'){ off = pi_next(off); continue; }
        if (p[0] != '\t') break;                  /* naechster Hersteller */
        if (p[1] != '\t'){
            if (pi_hexn(p+1, 4, &id) && id == (DWORD)dev) return off;
        }
        off = pi_next(off);
    }
    return PI_NOOFF;
}

static const char* PciIdsVendor(WORD ven)
{
    DWORD off = pi_venoff(ven);
    return (off == PI_NOOFF) ? NULL : pi_name(off);
}

static const char* PciIdsDevice(WORD ven, WORD dev)
{
    DWORD off = pi_devoff(ven, dev);
    return (off == PI_NOOFF) ? NULL : pi_name(off);
}

static const char* PciIdsSubsys(WORD ven, WORD dev, WORD sven, WORD sdev)
{
    DWORD off = pi_devoff(ven, dev), a, b;

    if (off == PI_NOOFF) return NULL;
    off = pi_next(off);

    while (off < gPiLen){
        const BYTE* p = gPiBuf + off;
        if (p[0] == 0 || p[0] == '#'){ off = pi_next(off); continue; }
        if (p[0] != '\t' || p[1] != '\t') break;  /* Block zu Ende */
        if (pi_hexn(p+2, 4, &a) && p[6]==' ' && pi_hexn(p+7, 4, &b)
            && a == (DWORD)sven && b == (DWORD)sdev)
            return pi_name(off);
        off = pi_next(off);
    }
    return NULL;
}

static const char* PciIdsClassName(BYTE base, BYTE sub)
{
    DWORD i, off = PI_NOOFF, id;
    const char* grob = NULL;

    if (!gPiBuf) return NULL;

    for (i=0;i<gPiClsN;i++)
        if (gPiCls[i].Id == (DWORD)base){ off = gPiCls[i].Off; break; }
    if (off == PI_NOOFF) return NULL;

    grob = pi_name(off);
    off  = pi_next(off);

    while (off < gPiLen){
        const BYTE* p = gPiBuf + off;
        if (p[0] == 0 || p[0] == '#'){ off = pi_next(off); continue; }
        if (p[0] != '\t') break;
        if (p[1] != '\t'){
            if (pi_hexn(p+1, 2, &id) && id == (DWORD)sub) return pi_name(off);
        }
        off = pi_next(off);
    }
    return grob;
}

static BOOL        PciIdsLoaded(void)  { return gPiBuf != NULL; }
static DWORD       PciIdsVendorCount(void) { return gPiVenN; }
static const char* PciIdsSource(void)  { return gPiPath[0] ? gPiPath : NULL; }

/*  Alles freigeben und neu suchen/einlesen (nach einer Aktualisierung).
    Achtung: alle vorher gelieferten Namenszeiger werden ungueltig --
    der Aufrufer baut den Baum danach neu auf.  */
static void PciIdsNeuLaden(void)
{
    if (gPiBuf) VirtualFree(gPiBuf, 0, MEM_RELEASE);
    gPiBuf = NULL; gPiLen = 0; gPiVenN = 0; gPiClsN = 0; gPiPath[0] = 0;
    PciIdsInit();
}

/*  Stand aus dem Dateikopf ("#\tVersion: 2026.09.25").  Nach pi_clean
    sind die Zeilen durch Null getrennt; gesucht wird im Kopf.  */
static void PciIdsStand(char* ziel, int max)
{
    DWORD i, grenze;
    ziel[0] = 0;
    if (!gPiBuf) return;
    grenze = gPiLen < 4096 ? gPiLen : 4096;
    for (i = 0; i + 8 < grenze; i++){
        if (gPiBuf[i]=='V' && gPiBuf[i+1]=='e' && gPiBuf[i+2]=='r' && gPiBuf[i+3]=='s' &&
            gPiBuf[i+4]=='i' && gPiBuf[i+5]=='o' && gPiBuf[i+6]=='n' && gPiBuf[i+7]==':'){
            int n = 0;
            i += 8;
            while (i < grenze && (gPiBuf[i]==' ' || gPiBuf[i]=='\t')) i++;
            while (i < grenze && gPiBuf[i] && gPiBuf[i] != '\r' && gPiBuf[i] != '\n' && n < max-1)
                ziel[n++] = (char)gPiBuf[i++];
            ziel[n] = 0;
            return;
        }
    }
}

#endif
