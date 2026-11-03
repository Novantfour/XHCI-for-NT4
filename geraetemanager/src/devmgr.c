/*++

    devmgr.c -- Geraetemanager fuer Windows NT 4.0

    Gebaut als DEVMGR.CPL -- ein Systemsteuerungselement.

    Eine .cpl-Datei ist nichts anderes als eine DLL, die CPlApplet
    exportiert.  Das Systemsteuerungsprogramm sucht beim Start alle
    .cpl-Dateien in system32, laedt sie und fragt ueber diese eine
    Funktion Name, Beschreibung und Symbol ab.

    Damit muss sysdm.cpl nicht angefasst werden -- eine eigene Datei
    danebenlegen genuegt, und sie erscheint automatisch.  Zum Entfernen
    reicht Loeschen.

    Dieselbe Uebersetzungseinheit baut auch DEVMGR.EXE; der Unterschied
    ist nur der Einstiegspunkt.

    Zeigt in einem Baum, was im Rechner steckt und welcher Treiber es
    bedient.  Die Angaben kommen aus vier Quellen:

        PCISCAN.SYS                 der PCI-Bus selbst, ueber HalGetBusData
        HKLM\HARDWARE\RESOURCEMAP   welcher Treiber welchen Bereich belegt
        Control\BootProfile         ob ein Treiber sein Geraet gefunden hat
        PCI.IDS                     Hersteller- und Geraetenamen im Klartext

    Die ersten beiden sind Serienausstattung jedes NT 4.0.  Das
    Startprofil schreibt nur der erweiterte Kernel; fehlt es, bleibt
    die Karte "Treiber" bei "keine Angaben".  PCI.IDS ist eine
    Textdatei, die man danebenlegt -- fehlt sie, zeigt der Baum die
    Zahlen wie zuvor.

    Aufbau:

        Nach Klasse geordnet, darunter die Geraete.  Rechts zu jedem
        ausgewaehlten Geraet die Einzelheiten: Kennung, Klasse,
        Basisadressen, zugeordneter Dienst, Startprofil.

    Gelesen und angezeigt wird alles; geschrieben wird nur an einer
    einzigen Stelle -- "Treiber einrichten" legt einen Dienst an.
    Immer mit Start = manuell, nie im Startablauf.

    Ohne C-Laufzeit uebersetzt, damit es auf einem NT 4.0 ohne
    Laufzeitbibliotheken laeuft.

--*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <winsvc.h>
#include <commdlg.h>

#include <winioctl.h>

/* drvpayload.h entfernt: der PCI-Treiber wird nicht mehr eingebettet und
   zur Laufzeit abgelegt (Dropper-Muster). PCISCAN.SYS liegt als Datei bei,
   den Dienst richtet USBINST ein. */
#include "pciids.h"
#include "ntusbioctl.h"      /* IOCTL_NTU_USBGERAETE (USB-Treiber usbnt) */

#define IOCTL_PCI_GET_LIST  0x00226004
#define SVCNAME             "PciScan"

#define RESMAP  "HARDWARE\\RESOURCEMAP"
#define PROFILE "SYSTEM\\CurrentControlSet\\Control\\BootProfile"

/* ---------------- ohne C-Laufzeit ---------------- */

void* memset(void* p, int c, unsigned int n)
{ BYTE* d=(BYTE*)p; while(n-->0) *d++=(BYTE)c; return p; }

void* memcpy(void* d, const void* s, unsigned int n)
{ BYTE* a=(BYTE*)d; const BYTE* b=(const BYTE*)s; while(n-->0) *a++=*b++; return d; }

static int slen(const char* s){int n=0;while(s[n])n++;return n;}
static void scpy(char* d,const char* s){int i=0;while(s[i]){d[i]=s[i];i++;}d[i]=0;}
static void scat(char* d,const char* s){int i=slen(d),j=0;while(s[j])d[i++]=s[j++];d[i]=0;}

#include "pcidl.h"
#include "sprache.h"      /* Deutsch/Englisch/Franzoesisch nach Region */         /* PCI.IDS per HTTP laden (WinSock) */

static void scatlim(char* d,const char* s,int max)
{int i=slen(d),j=0;while(s[j]&&j<max){d[i++]=s[j++];}d[i]=0;}

static char upper(char c){return (c>='a'&&c<='z')?(char)(c-32):c;}

static int ieq(const char* a,const char* b)
{
    int i=0;
    for(;;){
        char x=upper(a[i]), y=upper(b[i]);
        if(x!=y) return 0;
        if(x==0) return 1;
        i++;
    }
}

static int icontains(const char* hay,const char* need)
{
    int i=0;
    while(hay[i]){
        int j=0;
        while(need[j] && upper(hay[i+j])==upper(need[j])) j++;
        if(!need[j]) return 1;
        i++;
    }
    return 0;
}

static void hexinto(char* d, DWORD v, int digits)
{
    const char* h="0123456789ABCDEF";
    int i;
    for(i=digits-1;i>=0;i--){ d[i]=h[v&0xF]; v>>=4; }
    d[digits]=0;
}

static void numinto(char* d, DWORD v)
{
    char b[16]; int i=15;
    b[i--]=0;
    if(!v) b[i--]='0';
    while(v){ b[i--]=(char)('0'+v%10); v/=10; }
    scpy(d,&b[i+1]);
}

static void catnum(char* d, DWORD v){ char t[16]; numinto(t,v); scat(d,t); }
static void cathex(char* d, DWORD v, int n){ char t[12]; hexinto(t,v,n); scat(d,t); }

/* ---------------- Geraeteliste ---------------- */

#pragma pack(1)
typedef struct _PCI_ENTRY {
    DWORD  Bus, Device, Function;
    WORD   VendorID, DeviceID, SubVendorID, SubSystemID;
    BYTE   BaseClass, SubClass, RevisionID, HeaderType;
    DWORD  Bar[6];
    DWORD  InterruptLine;
} PCI_ENTRY;
#pragma pack()

#define MAXDEV 128

static PCI_ENTRY gDev[MAXDEV];
static DWORD     gDevCount = 0;
static char      gSvc[MAXDEV][32];

/*
    Woher die Zuordnung Geraet -> Treiber stammt.  Das war bisher
    nicht festgehalten, und genau deshalb liess sich nicht sagen,
    warum ein geladener Treiber nicht auftaucht.
*/

#define ZU_NICHTS   0
#define ZU_ADRESSE  1   /* ResourceMap, nur die Anfangsadresse */
#define ZU_GENAU    2   /* ResourceMap, Adresse und Busnummer  */
#define ZU_EINTRAG  3   /* am Dienstschluessel vermerkt        */

static BYTE      gSvcHow[MAXDEV];

/* ---------------- belegte Bereiche ---------------- */

#define MAXCLAIM 512

typedef struct _CLAIM {
    char  Service[32];
    DWORD Bus, Type, Start;
} CLAIM;

static CLAIM gClaim[MAXCLAIM];
static DWORD gClaimCount = 0;

static void ParseResourceList(const char* service, const BYTE* b, DWORD len)
{
    DWORD off=0, lists, i;
    if (len < 4) return;
    lists = *(const DWORD*)b;
    off = 4;
    for (i=0;i<lists;i++){
        DWORD bus, count, j;
        if (off+16 > len) return;
        bus   = *(const DWORD*)(b+off+4);
        count = *(const DWORD*)(b+off+12);
        off  += 16;
        for (j=0;j<count;j++){
            BYTE type; DWORD start;
            if (off+16 > len) return;
            type  = b[off];
            start = *(const DWORD*)(b+off+4);
            off  += 16;
            if ((type==1||type==3) && gClaimCount<MAXCLAIM){
                CLAIM* c=&gClaim[gClaimCount++];
                scpy(c->Service, service);
                c->Bus=bus; c->Type=type; c->Start=start;
            }
        }
    }
}

static void ReadOneDriver(const char* cls, const char* drv)
{
    char path[512]; HKEY k; DWORD idx=0;
    scpy(path,RESMAP); scat(path,"\\"); scat(path,cls);
    scat(path,"\\"); scat(path,drv);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,path,0,KEY_QUERY_VALUE,&k)!=ERROR_SUCCESS) return;
    for(;;){
        char name[256]; DWORD nlen=sizeof(name), type=0, dlen;
        static BYTE buf[4096];
        if (RegEnumValueA(k,idx,name,&nlen,NULL,&type,NULL,NULL)!=ERROR_SUCCESS) break;
        idx++;
        if (!icontains(name,".Raw")) continue;
        dlen=sizeof(buf);
        if (RegQueryValueExA(k,name,NULL,&type,buf,&dlen)!=ERROR_SUCCESS) continue;
        ParseResourceList(drv,buf,dlen);
    }
    RegCloseKey(k);
}

static void ReadResourceMap(void)
{
    HKEY root; DWORD ci=0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,RESMAP,0,KEY_ENUMERATE_SUB_KEYS,&root)!=ERROR_SUCCESS) return;
    for(;;){
        char cls[256]; DWORD clen=sizeof(cls); char cpath[512]; HKEY ck; DWORD di=0;
        if (RegEnumKeyExA(root,ci,cls,&clen,NULL,NULL,NULL,NULL)!=ERROR_SUCCESS) break;
        ci++;
        scpy(cpath,RESMAP); scat(cpath,"\\"); scat(cpath,cls);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,cpath,0,KEY_ENUMERATE_SUB_KEYS,&ck)!=ERROR_SUCCESS) continue;
        for(;;){
            char drv[256]; DWORD dlen=sizeof(drv);
            if (RegEnumKeyExA(ck,di,drv,&dlen,NULL,NULL,NULL,NULL)!=ERROR_SUCCESS) break;
            di++;
            ReadOneDriver(cls,drv);
        }
        RegCloseKey(ck);
    }
    RegCloseKey(root);
}

/*
    Zuordnung ueber Anfangsadresse und Busnummer -- auf der
    Vergleichsmaschine passte das fuenfmal von fuenf, ohne Toleranz.
    Interruptnummern taugen nicht: mehrere Geraete melden 128, der
    Tastaturcontroller meldet 0xFFFFFFFF.
*/

#define SVCROOT "SYSTEM\\CurrentControlSet\\Services"

/* vier Hexziffern an einer Stelle im Text */

static BOOL HexAb(const char* s, DWORD* out)
{
    DWORD v=0; int i, d;
    for(i=0;i<4;i++){
        char c=s[i];
        if(c>='0'&&c<='9') d=c-'0';
        else if(c>='a'&&c<='f') d=c-'a'+10;
        else if(c>='A'&&c<='F') d=c-'A'+10;
        else return FALSE;
        v=(v<<4)|(DWORD)d;
    }
    *out=v;
    return TRUE;
}

static BOOL IdAus(const char* s, DWORD* ven, DWORD* dev)
{
    int i;
    BOOL hv=FALSE, hd=FALSE;
    for(i=0;s[i];i++){
        if(!hv && s[i]=='V'&&s[i+1]=='E'&&s[i+2]=='N'&&s[i+3]=='_')
            hv=HexAb(s+i+4,ven);
        if(!hd && s[i]=='D'&&s[i+1]=='E'&&s[i+2]=='V'&&s[i+3]=='_')
            hd=HexAb(s+i+4,dev);
    }
    return hv && hd;
}

/*
    Zweite Beweisquelle.  Jeder Treiber, der ueber "Treiber
    einrichten" angelegt wurde, traegt am Dienstschluessel den Wert
    DevMgrGeraet mit der PCI-Kennung.  Das ist eine ausdrueckliche
    Angabe und braucht keinen Adressvergleich -- sie gilt auch dann,
    wenn der Treiber gar keine Bereiche belegt oder sie unter einem
    Namen eintraegt, den wir nicht finden.
*/

static void MatchAusRegistry(void)
{
    HKEY root, k;
    DWORD idx=0;

    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,SVCROOT,0,
                     KEY_ENUMERATE_SUB_KEYS,&root)!=ERROR_SUCCESS) return;
    for(;;){
        char name[256], path[512], wert[160];
        DWORD nlen=sizeof(name), wlen, typ=0, ven, dev, i;

        if(RegEnumKeyExA(root,idx,name,&nlen,NULL,NULL,NULL,NULL)!=ERROR_SUCCESS) break;
        idx++;

        scpy(path,SVCROOT); scat(path,"\\"); scatlim(path,name,200);
        if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,path,0,KEY_QUERY_VALUE,&k)!=ERROR_SUCCESS)
            continue;

        wlen=sizeof(wert)-1;
        if(RegQueryValueExA(k,"DevMgrGeraet",NULL,&typ,(LPBYTE)wert,&wlen)==ERROR_SUCCESS
           && typ==REG_SZ){
            wert[sizeof(wert)-1]=0;
            if(IdAus(wert,&ven,&dev)){
                for(i=0;i<gDevCount;i++){
                    if(gDev[i].VendorID==(WORD)ven && gDev[i].DeviceID==(WORD)dev){
                        scpy(gSvc[i],name);
                        gSvcHow[i]=ZU_EINTRAG;
                    }
                }
            }
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
}

/*
    Der Adressvergleich laeuft jetzt in zwei Durchgaengen.  Der erste
    verlangt Adresse und Busnummer, der zweite nur die Adresse -- es
    gibt Treiber, die ihre Bereiche mit Busnummer 0 eintragen,
    gleichgueltig auf welchem Bus das Geraet wirklich sitzt.  Welcher
    Durchgang gegriffen hat, steht in gSvcHow und wird auf der Karte
    "Treiber" auch angezeigt.
*/

static void Match(void)
{
    DWORD i,j,b;
    int durchgang;

    for(i=0;i<gDevCount;i++){ gSvc[i][0]=0; gSvcHow[i]=ZU_NICHTS; }

    for(durchgang=0;durchgang<2;durchgang++){
        for(i=0;i<gDevCount;i++){
            if(gSvc[i][0]) continue;
            for(b=0;b<6 && !gSvc[i][0];b++){
                DWORD bar=gDev[i].Bar[b], addr, want;
                if(!bar) continue;
                if(bar&1){ addr=bar&0xFFFFFFFC; want=1; }
                else     { addr=bar&0xFFFFFFF0; want=3; }
                for(j=0;j<gClaimCount;j++){
                    if(gClaim[j].Type!=want) continue;
                    if(gClaim[j].Start!=addr) continue;
                    if(durchgang==0 && gClaim[j].Bus!=gDev[i].Bus) continue;
                    scpy(gSvc[i],gClaim[j].Service);
                    gSvcHow[i]=(durchgang==0)?ZU_GENAU:ZU_ADRESSE;
                    break;
                }
            }
        }
    }

    MatchAusRegistry();
}

/* ---------------- Namen ---------------- */

static const char* ClassNameFest(BYTE base, BYTE sub)
{
    switch(base){
    case 0x00: return "Nicht klassifiziert";
    case 0x01:
        switch(sub){
        case 0x00: return "Massenspeicher, SCSI";
        case 0x01: return "Massenspeicher, IDE";
        case 0x06: return "Massenspeicher, SATA";
        }
        return "Massenspeicher";
    case 0x02: return (sub==0) ? "Netzwerk, Ethernet" : "Netzwerk";
    case 0x03: return "Grafikkarte";
    case 0x04:
        if(sub==0x01) return "Audio";
        if(sub==0x03) return "Audio, HD";
        return "Multimedia";
    case 0x05: return "Speichercontroller";
    case 0x06:
        if(sub==0x00) return "Bruecke, Host";
        if(sub==0x01) return "Bruecke, ISA";
        if(sub==0x04) return "Bruecke, PCI-PCI";
        return "Bruecke";
    case 0x07: return "Kommunikation";
    case 0x08: return "Systembaustein";
    case 0x09: return "Eingabegeraet";
    case 0x0B: return "Prozessor";
    case 0x0C:
        if(sub==0x03) return "USB-Controller";
        if(sub==0x05) return "SMBus";
        return "Serieller Bus";
    case 0x0D: return "Funk";
    }
    return NULL;
}

/*
    Die eingebauten deutschen Namen haben Vorrang -- sie fassen
    grueber zusammen und ergeben einen aufgeraeumteren Baum.  Nur
    was dort fehlt, kommt aus PCI.IDS.
*/

static const char* ClassName(BYTE base, BYTE sub)
{
    const char* n = ClassNameFest(base, sub);
    if (n) return n;
    n = PciIdsClassName(base, sub);
    return n ? n : "Unbekannt";
}

/*
    Zuerst PCI.IDS, dann die eingebaute Notliste.  Die Notliste
    bleibt, damit das Programm auch ohne die Datei etwas anzeigt.
*/

static const char* VendorName(WORD v)
{
    const char* n = PciIdsVendor(v);
    if (n) return n;

    switch(v){
    case 0x1002: return "ATI/AMD";
    case 0x1022: return "AMD";
    case 0x10DE: return "nVidia";
    case 0x10EC: return "Realtek";
    case 0x1102: return "Creative";
    case 0x1106: return "VIA";
    case 0x11AB: return "Marvell";
    case 0x1969: return "Atheros";
    case 0x197B: return "JMicron";
    case 0x8086: return "Intel";
    case 0x9004:
    case 0x9005: return "Adaptec";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    case 0x1013: return "Cirrus Logic";
    case 0x1014: return "IBM";
    case 0x1033: return "NEC";
    case 0x103C: return "Hewlett-Packard";
    case 0x1039: return "SiS";
    case 0x104C: return "Texas Instruments";
    case 0x105A: return "Promise";
    case 0x1077: return "QLogic";
    case 0x108E: return "Sun";
    case 0x10B5: return "PLX";
    case 0x10B7: return "3Com";
    case 0x10B9: return "ALi/ULi";
    case 0x1095: return "Silicon Image";
    case 0x1131: return "Philips";
    case 0x1179: return "Toshiba";
    case 0x11C1: return "LSI/Agere";
    case 0x1180: return "Ricoh";
    case 0x1274: return "Ensoniq";
    case 0x1283: return "ITE";
    case 0x12AB: return "YUAN";
    case 0x1317: return "ADMtek";
    case 0x1385: return "Netgear";
    case 0x14E4: return "Broadcom";
    case 0x14F1: return "Conexant";
    case 0x1524: return "ENE";
    case 0x1558: return "Clevo";
    case 0x1737: return "Linksys";
    case 0x168C: return "Atheros";
    case 0x1814: return "Ralink";
    case 0x1B21: return "ASMedia";
    case 0x1B4B: return "Marvell";
    case 0x1B73: return "Fresco Logic";
    case 0x1AF4: return "Red Hat/virtio";
    case 0x1B36: return "Red Hat/QEMU";
    case 0x5333: return "S3";
    }
    return "";
}

/*
    Geraetename.  Leerstring, wenn PCI.IDS fehlt oder den Eintrag
    nicht kennt -- der Aufrufer faellt dann auf die Zahlen zurueck.
*/

static const char* DeviceName(WORD ven, WORD dev)
{
    const char* n = PciIdsDevice(ven, dev);
    return n ? n : "";
}

static const char* SubsysName(WORD ven, WORD dev, WORD sven, WORD sdev)
{
    const char* n;
    if (!sven && !sdev) return "";
    n = PciIdsSubsys(ven, dev, sven, sdev);
    return n ? n : "";
}

/* ---------------- Treiber laden ---------------- */

static BOOL gPciFehlt = FALSE;   /* PCISCAN nicht erreichbar: PCI-Liste leer */

static BOOL GetList(void)
{
    HANDLE dev=INVALID_HANDLE_VALUE;
    DWORD got=0;
    BOOL ok=FALSE;
    static BYTE buf[sizeof(DWORD)+MAXDEV*sizeof(PCI_ENTRY)];

    /*  Frueher legte diese Funktion den PCI-Lesetreiber selbst nach
     *  system32\drivers, erzeugte einen Dienst und startete ihn -- das ist
     *  ein Dropper-Muster, das Virenscanner (Bkav, Kingsoft, Trellix) an
     *  der CPL/EXE zu Recht misstrauen.  Jetzt wird nur noch ein bereits
     *  eingerichteter Dienst BENUTZT.  Das Einrichten macht USBINST einmalig
     *  (wie beim USB-Treiber); PCISCAN.SYS liegt als normale Datei bei.  */

    dev=CreateFileA("\\\\.\\PciScan",GENERIC_READ,
                    FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(dev==INVALID_HANDLE_VALUE){
        /*  Dienst nicht da/nicht gestartet -- PCI-Liste bleibt leer, USB
         *  und alles andere funktioniert weiter.  */
        gDevCount=0;
        return FALSE;
    }
    if(DeviceIoControl(dev,IOCTL_PCI_GET_LIST,NULL,0,buf,sizeof(buf),&got,NULL)){
        gDevCount=*(DWORD*)buf;
        if(gDevCount>MAXDEV) gDevCount=MAXDEV;
        memcpy(gDev,buf+sizeof(DWORD),gDevCount*sizeof(PCI_ENTRY));
        ok=TRUE;
    }
    CloseHandle(dev);
    return ok;
}

/* ---------------- Startprofil eines Dienstes ---------------- */

/*
    Was der Kernel beim letzten Start ueber diesen Treiber notiert hat:
    Status, Ladedauer, Fehlschlaege in Folge.
*/

static DWORD ProfileDword(const char* svc, const char* name, DWORD dflt)
{
    char path[256]; HKEY k; DWORD type, val=0, len=sizeof(DWORD);
    if(!svc[0]) return dflt;
    scpy(path,PROFILE); scat(path,"\\"); scat(path,svc);
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,path,0,KEY_QUERY_VALUE,&k)!=ERROR_SUCCESS)
        return dflt;
    if(RegQueryValueExA(k,name,NULL,&type,(LPBYTE)&val,&len)!=ERROR_SUCCESS
       || type!=REG_DWORD) val=dflt;
    RegCloseKey(k);
    return val;
}

/* ------------------------------------------------------------------ */
/*  Oberflaeche                                                         */
/* ------------------------------------------------------------------ */

/*
    Nach Windows-Art: links ein Baum mit Symbolen, unten Schaltflaechen,
    und zu jedem Geraet ein Eigenschaftenblatt mit Registerkarten --
    Allgemein, Treiber, Ressourcen.

    Der erste Wurf hatte Baum und Textfeld nebeneinander.  Das ist zwar
    uebersichtlich, sieht aber nicht aus wie Windows.
*/

#define IDC_TREE    100
#define IDC_PROPS   101
#define IDC_REFRESH 102
#define IDC_CLOSE   103
#define IDC_INSTALL 104
#define IDC_DUMP    105
#define IDC_PCIIDS  106

static HWND      gMain, gTree, gBtnProps, gBtnInstall, gBtnRefresh, gBtnClose, gBtnDump, gBtnIds;
static HINSTANCE gInst;
static HIMAGELIST gImages;
static HFONT     gFont;

/* Bildindizes in der Liste */
#define IMG_COMPUTER 0
#define IMG_CLASS    1
#define IMG_DEVICE   2
#define IMG_WARN     3
#define IMG_USB      4
#define IMG_HUB      5

/*  Symbole in der Ressource (devmgr.rc).  */
#define ICO_APP      1
#define ICO_RECHNER  2
#define ICO_KLASSE   3
#define ICO_GERAET   4
#define ICO_WARNUNG  5
#define ICO_USB      6
#define ICO_HUB      7

/*  Baumkennungen: PCI-Geraete 0..gDevCount-1, USB-Geraete ab USB_BASIS.
    Alles, was mit PCI arbeitet, prueft gSel < gDevCount -- USB-Eintraege
    fallen dort von selbst heraus.  */
#define USB_BASIS    0x1000

static int gSel = -1;

/* ---------------- Symbole ---------------- */

/*
    Eigene Symbole braeuchten eine Ressourcendatei.  Bis dahin dienen
    Standardsymbole des Systems -- sie sehen an dieser Stelle richtig
    aus und kosten nichts.
*/

static void MakeImages(void)
{
    static const int ids[6]={ICO_RECHNER,ICO_KLASSE,ICO_GERAET,ICO_WARNUNG,ICO_USB,ICO_HUB};
    static const LPCSTR ersatz[6]={IDI_APPLICATION,IDI_INFORMATION,IDI_APPLICATION,
                                   IDI_EXCLAMATION,IDI_APPLICATION,IDI_APPLICATION};
    int i;

    /*  256 Farben (ILC_COLOR8); die eigenen Symbole liegen in der
        Ressource, mit 16-Farben-Fassung fuer alte Grafikmodi.  */
    gImages = ImageList_Create(16,16, ILC_COLOR8 | ILC_MASK, 6, 0);
    if(!gImages) return;
    for(i=0;i<6;i++){
        HICON h=(HICON)LoadImageA(gInst,MAKEINTRESOURCEA(ids[i]),IMAGE_ICON,16,16,0);
        if(!h) h=LoadIconA(NULL,ersatz[i]);
        ImageList_AddIcon(gImages,h);
    }
}

/* ---------------- Dienstzustand ---------------- */

/*
    Zwei Angaben, die bisher fehlten und die die Frage "der Treiber
    ist doch geladen" ueberhaupt erst beantwortbar machen:
    der Startwert aus der Registry und der Zustand beim
    Dienstverwalter.
*/

static DWORD DienstStart(const char* dienst)
{
    char path[320]; HKEY k; DWORD typ, val=0xFFFFFFFF, len=sizeof(DWORD);
    if(!dienst[0]) return 0xFFFFFFFF;
    scpy(path,SVCROOT); scat(path,"\\"); scatlim(path,dienst,200);
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,path,0,KEY_QUERY_VALUE,&k)!=ERROR_SUCCESS)
        return 0xFFFFFFFF;
    if(RegQueryValueExA(k,"Start",NULL,&typ,(LPBYTE)&val,&len)!=ERROR_SUCCESS
       || typ!=REG_DWORD) val=0xFFFFFFFF;
    RegCloseKey(k);
    return val;
}

static const char* StartName(DWORD s)
{
    switch(s){
    case 0: return "beim Bootvorgang (0)";
    case 1: return "beim Systemstart (1)";
    case 2: return "automatisch (2)";
    case 3: return "manuell (3)";
    case 4: return "deaktiviert (4)";
    }
    return "kein Eintrag";
}

#ifdef NUR_ANSEHEN
/*  Schlanke CPL: keine SC-Manager-Aufrufe (Virenscanner-freundlich).
    Der Dienstzustand wird dann nicht angezeigt -- die Geraeteanzeige
    braucht ihn nicht.  */
static DWORD DienstZustand(const char* dienst)
{
    (void)dienst;
    return 0;
}
#else
static DWORD DienstZustand(const char* dienst)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS ss;
    DWORD z=0;

    if(!dienst[0]) return 0;
    scm=OpenSCManagerA(NULL,NULL,SC_MANAGER_CONNECT);
    if(!scm) return 0;
    svc=OpenServiceA(scm,dienst,SERVICE_QUERY_STATUS);
    if(svc){
        if(QueryServiceStatus(svc,&ss)) z=ss.dwCurrentState;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return z;
}
#endif /* NUR_ANSEHEN */

static const char* ZustandName(DWORD z)
{
    switch(z){
    case 1: return "angehalten";
    case 2: return "startet";
    case 3: return "haelt an";
    case 4: return "geladen";
    case 5: return "wird fortgesetzt";
    case 6: return "pausiert";
    case 7: return "pausiert";
    }
    return "nicht abfragbar";
}

static const char* HerkunftName(BYTE h)
{
    switch(h){
    case ZU_EINTRAG: return "Eintrag am Dienstschluessel";
    case ZU_GENAU:   return "ResourceMap, Adresse und Bus";
    case ZU_ADRESSE: return "ResourceMap, nur Adresse";
    }
    return "keine";
}

/* ---------------- Baum ---------------- */

/* ---------------- USB-Geraete (lsusb) ---------------- */

/*
    Der USB-Treiber (usbnt, oder eine seiner Bauarten) liefert mit
    IOCTL_NTU_USBGERAETE jedes USB-Geraet einmal: Deskriptor, Texte,
    Ort im Baum und je Schnittstelle Klasse und Treiber.  Fehlt der
    Treiber, gibt es keine Klasse S(T_K_USB) -- kein Fehler.
*/

static NTU_USB_ANTWORT gUsb;
static BOOL            gUsbOk;
static char            gUsbQuelle[16];
static int             gUsbSel = -1;

static void UsbLesen(void)
{
    static const char* namen[5]={"\\\\.\\UsbNt","\\\\.\\XhciNt","\\\\.\\EhciNt",
                                 "\\\\.\\OhciNt","\\\\.\\UhciNt"};
    int i; DWORD n;

    gUsbOk=FALSE; gUsb.Anzahl=0; gUsbQuelle[0]=0;
    for(i=0;i<5 && !gUsbOk;i++){
        HANDLE h=CreateFileA(namen[i],GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                             NULL,OPEN_EXISTING,0,NULL);
        if(h==INVALID_HANDLE_VALUE) continue;
        memset(&gUsb,0,sizeof(gUsb));
        if(DeviceIoControl(h,IOCTL_NTU_USBGERAETE,NULL,0,&gUsb,sizeof(gUsb),&n,NULL)
           && n>=sizeof(gUsb) && gUsb.Anzahl<=NTU_USB_MAX){
            gUsbOk=TRUE; scpy(gUsbQuelle,namen[i]+4);
        }
        CloseHandle(h);
    }
}

static const char* UsbKlasse(BYTE k, BYTE u, BYTE p)
{
    switch(k){
    case 0x00: return "je Schnittstelle";
    case 0x01: return "Audio";
    case 0x02: return "Kommunikation";
    case 0x03:
        if(u==1 && p==1) return "Eingabe (Tastatur)";
        if(u==1 && p==2) return "Eingabe (Maus)";
        return "Eingabe (HID)";
    case 0x05: return "Physisch";
    case 0x06: return "Bild (Kamera, Scanner)";
    case 0x07: return "Drucker";
    case 0x08: return "Massenspeicher";
    case 0x09: return "Hub";
    case 0x0A: return "Kommunikation (Daten)";
    case 0x0B: return "Chipkarte";
    case 0x0D: return "Inhaltsschutz";
    case 0x0E: return "Video";
    case 0x0F: return "Gesundheit";
    case 0x10: return "Audio/Video";
    case 0xDC: return "Diagnose";
    case 0xE0: return "Funk (Bluetooth)";
    case 0xEF: return "Verschiedenes";
    case 0xFE: return "anwendungsspezifisch";
    case 0xFF: return "herstellerspezifisch";
    }
    return S(T_UNBEKANNT);
}

static const char* UsbTempo(BYTE g)
{
    switch(g){
    case 1: return S(T_LOWSPEED);
    case 2: return S(T_FULLSPEED);
    case 3: return S(T_HIGHSPEED);
    case 4: return S(T_SUPERSPEED);
    }
    return S(T_UNBEKANNT);
}

static void catbcd(char* d, WORD v)
{
    catnum(d,(v>>12)*10+((v>>8)&15)); scat(d,".");
    cathex(d,(v>>4)&15,1); cathex(d,v&15,1);
}

/*  Anzeigename: "Hersteller Produkt", sonst Klasse und Kennung.  */
static void UsbName(const NTU_USB_GERAET* g, char* z, int max)
{
    z[0]=0;
    if(g->Tiefe==0){
        scat(z,S(T_USB_WURZELHUB));
        if(g->HostArt[0]){ scat(z," ("); scat(z,g->HostArt); scat(z,")"); }
        scat(z,", Bus "); catnum(z,g->Bus+1);
        return;
    }
    if(g->Produkt[0]){
        if(g->Hersteller[0] && !icontains(g->Produkt,g->Hersteller)){
            scatlim(z,g->Hersteller,40); scat(z," ");
        }
        scatlim(z,g->Produkt,max-48);
    } else {
        scat(z,g->Hub ? S(T_USB_HUB) : S(T_USB_GERAET));
        if(g->Hersteller[0]){ scat(z," von "); scatlim(z,g->Hersteller,40); }
    }
    scat(z,"  ["); cathex(z,g->Vendor,4); scat(z,":"); cathex(z,g->Product,4); scat(z,"]");
}

/*  Haengt an keiner Schnittstelle ein Treiber?  (Hubs zaehlen nicht.)  */
static BOOL UsbOhneTreiber(const NTU_USB_GERAET* g)
{
    int i;
    if(g->Hub || g->Tiefe==0) return FALSE;
    for(i=0;i<g->AnzahlIf;i++) if(g->If[i].Treiber[0]) return FALSE;
    return TRUE;
}

static void UsbEinfuegen(HTREEITEM eltern, int bus, int adresse, int tiefe)
{
    DWORD i;
    if(tiefe>7) return;
    for(i=0;i<gUsb.Anzahl;i++){
        const NTU_USB_GERAET* g=&gUsb.Geraet[i];
        TV_INSERTSTRUCT is; HTREEITEM h; char z[200]; int bild;
        if(g->Bus!=bus || g->Tiefe==0 || g->ElternAdresse!=adresse) continue;
        UsbName(g,z,sizeof(z));
        bild = g->Hub ? IMG_HUB : (UsbOhneTreiber(g) ? IMG_WARN : IMG_USB);
        memset(&is,0,sizeof(is));
        is.hParent=eltern; is.hInsertAfter=TVI_LAST;
        is.item.mask=TVIF_TEXT|TVIF_PARAM|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
        is.item.pszText=z; is.item.lParam=(LPARAM)(USB_BASIS+i);
        is.item.iImage=bild; is.item.iSelectedImage=bild;
        h=TreeView_InsertItem(gTree,&is);
        if(g->Hub){
            UsbEinfuegen(h,bus,g->Adresse,tiefe+1);
            TreeView_Expand(gTree,h,TVE_EXPAND);
        }
    }
}

static void UsbBaum(HTREEITEM root)
{
    TV_INSERTSTRUCT is; HTREEITEM klasse; DWORD i; char t[64];
    UsbLesen();                 /* bei jedem Aufbau: Start, Aktualisieren */
    if(!gUsbOk) return;
    scpy(t,S(T_K_USB));
    memset(&is,0,sizeof(is));
    is.hParent=root; is.hInsertAfter=TVI_LAST;
    is.item.mask=TVIF_TEXT|TVIF_PARAM|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
    is.item.pszText=t; is.item.lParam=-1;
    is.item.iImage=IMG_USB; is.item.iSelectedImage=IMG_USB;
    klasse=TreeView_InsertItem(gTree,&is);
    for(i=0;i<gUsb.Anzahl;i++){
        const NTU_USB_GERAET* g=&gUsb.Geraet[i];
        HTREEITEM h; char z[200];
        if(g->Tiefe!=0) continue;
        UsbName(g,z,sizeof(z));
        memset(&is,0,sizeof(is));
        is.hParent=klasse; is.hInsertAfter=TVI_LAST;
        is.item.mask=TVIF_TEXT|TVIF_PARAM|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
        is.item.pszText=z; is.item.lParam=(LPARAM)(USB_BASIS+i);
        is.item.iImage=IMG_HUB; is.item.iSelectedImage=IMG_HUB;
        h=TreeView_InsertItem(gTree,&is);
        UsbEinfuegen(h,g->Bus,g->Adresse,1);
        TreeView_Expand(gTree,h,TVE_EXPAND);
    }
    TreeView_Expand(gTree,klasse,TVE_EXPAND);
}

static void BuildTree(void)
{
    DWORD i,j;
    static BYTE done[MAXDEV];
    TV_INSERTSTRUCT is;
    HTREEITEM root;
    char rechner[80]; DWORD rl=sizeof(rechner);

    memset(done,0,sizeof(done));
    TreeView_DeleteAllItems(gTree);

    if(!GetComputerNameA(rechner,&rl)) scpy(rechner,"Dieser Rechner");

    memset(&is,0,sizeof(is));
    is.hParent=TVI_ROOT; is.hInsertAfter=TVI_LAST;
    is.item.mask=TVIF_TEXT|TVIF_PARAM|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
    is.item.pszText=rechner;
    is.item.lParam=-1;
    is.item.iImage=IMG_COMPUTER;
    is.item.iSelectedImage=IMG_COMPUTER;
    root=TreeView_InsertItem(gTree,&is);

    if(gPciFehlt){
        static char hinweis[] = "PCI-Geraete nicht verfuegbar: Dienst PciScan laeuft nicht "
                                "(USBINST ein richtet ihn ein, dann Aktualisieren)";
        memset(&is,0,sizeof(is));
        is.hParent=root; is.hInsertAfter=TVI_LAST;
        is.item.mask=TVIF_TEXT|TVIF_PARAM|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
        is.item.pszText=hinweis;
        is.item.lParam=-1;
        is.item.iImage=IMG_WARN;
        is.item.iSelectedImage=IMG_WARN;
        TreeView_InsertItem(gTree,&is);
    }

    for(i=0;i<gDevCount;i++){
        HTREEITEM klasse;
        const char* kn;
        char kbuf[128];

        if(done[i]) continue;
        kn=ClassName(gDev[i].BaseClass,gDev[i].SubClass);
        kbuf[0]=0; scatlim(kbuf,kn,120);

        memset(&is,0,sizeof(is));
        is.hParent=root; is.hInsertAfter=TVI_LAST;
        is.item.mask=TVIF_TEXT|TVIF_PARAM|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
        is.item.pszText=kbuf;
        is.item.lParam=-1;
        is.item.iImage=IMG_CLASS;
        is.item.iSelectedImage=IMG_CLASS;
        klasse=TreeView_InsertItem(gTree,&is);

        for(j=i;j<gDevCount;j++){
            char zeile[256];
            const char* h;
            const char* g;
            int ohne;

            if(done[j]) continue;
            if(!ieq(ClassName(gDev[j].BaseClass,gDev[j].SubClass),kn)) continue;
            done[j]=1;

            ohne = (!gSvc[j][0] && gDev[j].BaseClass!=0x06);

            /*
                Klartext, wenn PCI.IDS ihn hergibt; sonst wie bisher
                die Zahlen.  Der Herstellername wird vorangestellt,
                weil die Datenbank ihn im Geraetenamen weglaesst.
            */

            zeile[0]=0;
            h=VendorName(gDev[j].VendorID);
            g=DeviceName(gDev[j].VendorID,gDev[j].DeviceID);

            if(g[0]){
                if(h[0]){ scatlim(zeile,h,48); scat(zeile," "); }
                scatlim(zeile,g,150);
            } else {
                if(h[0]){ scatlim(zeile,h,48); scat(zeile," "); }
                cathex(zeile,gDev[j].VendorID,4);
                scat(zeile,":");
                cathex(zeile,gDev[j].DeviceID,4);
            }

            memset(&is,0,sizeof(is));
            is.hParent=klasse; is.hInsertAfter=TVI_LAST;
            is.item.mask=TVIF_TEXT|TVIF_PARAM|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
            is.item.pszText=zeile;
            is.item.lParam=(LPARAM)j;
            is.item.iImage= ohne ? IMG_WARN : IMG_DEVICE;
            is.item.iSelectedImage= ohne ? IMG_WARN : IMG_DEVICE;
            TreeView_InsertItem(gTree,&is);
        }
        TreeView_Expand(gTree,klasse,TVE_EXPAND);
    }
    UsbBaum(root);
    TreeView_Expand(gTree,root,TVE_EXPAND);
}

/* ---------------- Eigenschaftenblatt ---------------- */

/*
    Die Seiten werden ohne Dialogvorlage gebaut: DLGTEMPLATE im
    Speicher, Steuerelemente zur Laufzeit erzeugt.  Das erspart eine
    Ressourcendatei und damit den Umweg ueber rc.exe.
*/

/*
    Wer DS_SETFONT setzt, muss hinter dem Titel auch Punktgroesse und
    Schriftname liefern -- sonst liest der Dialogverwalter beides aus
    dem, was zufaellig hinter der Struktur liegt.  Im ersten Entwurf
    war das Fuellung, also Groesse 0 und leerer Name; die Dialogein-
    heiten wurden daraus falsch berechnet.  Jetzt steht es da.
*/

#pragma pack(push, 4)
typedef struct {
    DWORD style, dwExtendedStyle;
    WORD  cdit;
    short x, y, cx, cy;
    WORD  menu, windowClass, title;
    WORD  pointsize;
    WCHAR face[14];
} DLGTPL;
#pragma pack(pop)

/*
    NICHT const.  comctl32 aendert die Vorlage an Ort und Stelle,
    bevor es den Dialog erzeugt -- es setzt WS_CHILD und DS_CONTROL
    und nimmt WS_CAPTION und WS_SYSMENU heraus.  Liegt die Vorlage
    in .rdata, schreibt es in eine nur lesbare Seite und die
    Ausnahme faellt in comctl32 an, mit der Adresse der Vorlage als
    zweitem Wert.  Sie muss in .data liegen.
*/

static DLGTPL gTpl = {
    WS_CHILD|WS_VISIBLE|DS_SETFONT|DS_3DLOOK, 0,
    0,                       /* cdit -- Steuerelemente kommen zur Laufzeit */
    0,0, 300,200,
    0,0,0,                   /* kein Menue, keine Klasse, kein Titel */
    8,
    {'M','S',' ','S','a','n','s',' ','S','e','r','i','f',0}
};

static void AddLine(HWND page, int* y, const char* label, const char* wert)
{
    HWND h;
    h=CreateWindowExA(0,"STATIC",label,WS_CHILD|WS_VISIBLE|SS_NOPREFIX,
                      10,*y,100,14,page,NULL,gInst,NULL);
    SendMessageA(h,WM_SETFONT,(WPARAM)gFont,TRUE);
    h=CreateWindowExA(0,"STATIC",wert,WS_CHILD|WS_VISIBLE|SS_NOPREFIX,
                      115,*y,330,14,page,NULL,gInst,NULL);
    SendMessageA(h,WM_SETFONT,(WPARAM)gFont,TRUE);
    *y += 18;
}

static void AddText(HWND page, int y, int h, const char* text)
{
    HWND e=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",text,
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY,
            10,y,435,h,page,NULL,gInst,NULL);
    SendMessageA(e,WM_SETFONT,(WPARAM)gFont,TRUE);
}

static BOOL CALLBACK PageAllgemein(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if(msg==WM_INITDIALOG){
        PCI_ENTRY* e=&gDev[gSel];
        char t[320];
        int y=12;
        const char* h;
        const char* g;
        const char* u;

        h=VendorName(e->VendorID);
        g=DeviceName(e->VendorID,e->DeviceID);
        u=SubsysName(e->VendorID,e->DeviceID,e->SubVendorID,e->SubSystemID);

        AddLine(hwnd,&y,S(T_GERAETETYP), ClassName(e->BaseClass,e->SubClass));
        AddLine(hwnd,&y,S(T_HERSTELLER), h[0]?h:S(T_UNBEKANNT));

        t[0]=0; scatlim(t,g[0]?g:S(T_UNBEKANNT),300);
        AddLine(hwnd,&y,S(T_BEZEICHNUNG),t);

        if(u[0]){
            t[0]=0; scatlim(t,u,300);
            AddLine(hwnd,&y,"Untersystem:",t);
        }

        t[0]=0; scat(t,"Bus "); catnum(t,e->Bus);
        scat(t,", Steckplatz "); catnum(t,e->Device);
        scat(t,", Funktion "); catnum(t,e->Function);
        AddLine(hwnd,&y,S(T_ORT),t);

        t[0]=0; cathex(t,e->BaseClass,2); cathex(t,e->SubClass,2);
        AddLine(hwnd,&y,S(T_KLASSENCODE),t);

        t[0]=0; cathex(t,e->RevisionID,2);
        AddLine(hwnd,&y,S(T_REVISION),t);

        y += 8;

        t[0]=0;
        scat(t,"PCI\\VEN_"); cathex(t,e->VendorID,4);
        scat(t,"&DEV_"); cathex(t,e->DeviceID,4);
        if(e->SubVendorID||e->SubSystemID){
            scat(t,"&SUBSYS_"); cathex(t,e->SubSystemID,4); cathex(t,e->SubVendorID,4);
        }
        AddLine(hwnd,&y,S(T_KENNUNG),"");
        AddText(hwnd,y-18+16,20,t);
        y += 26;

        {
            const char* st;
            if(gSvc[gSel][0])
                st="Das Geraet wird von einem Treiber bedient.";
            else if(e->BaseClass==0x06)
                st="Bruecke -- braucht keinen Treiber.";
            else
                st="Diesem Geraet ist kein Treiber zugeordnet.";
            AddText(hwnd,y,34,st);
        }
        return TRUE;
    }
    return FALSE;
}

static BOOL CALLBACK PageTreiber(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if(msg==WM_INITDIALOG){
        char t[1024];
        int y=12;

        AddLine(hwnd,&y,"Dienst:", gSvc[gSel][0] ? gSvc[gSel] : S(T_KEIN_TREIBER));

        if(gSvc[gSel][0]){
            AddLine(hwnd,&y,"Zustand:",   ZustandName(DienstZustand(gSvc[gSel])));
            AddLine(hwnd,&y,"Start:",     StartName(DienstStart(gSvc[gSel])));
            AddLine(hwnd,&y,"Zuordnung:", HerkunftName(gSvcHow[gSel]));
        }

        if(gSvc[gSel][0]){
            DWORD st=ProfileDword(gSvc[gSel],"LastStatus",0xFFFFFFFF);
            DWORD ms=ProfileDword(gSvc[gSel],"LastMs",0xFFFFFFFF);
            DWORD fc=ProfileDword(gSvc[gSel],"FailCount",0xFFFFFFFF);

            if(st!=0xFFFFFFFF){
                t[0]=0; cathex(t,st,8);
                if(st==0) scat(t,"   (geladen)");
                AddLine(hwnd,&y,"Letzter Status:",t);
                t[0]=0; catnum(t,ms); scat(t," ms");
                AddLine(hwnd,&y,"Ladedauer:",t);
                t[0]=0; catnum(t,fc);
                AddLine(hwnd,&y,"Fehlschlaege:",t);
            } else {
                AddLine(hwnd,&y,"Startprofil:","keine Angaben");
            }
        }

        y += 10;

        t[0]=0;
        switch(gSvcHow[gSel]){
        case ZU_EINTRAG:
            scat(t,"Der Dienstschluessel dieses Treibers traegt den Wert "
                   "DevMgrGeraet mit der Kennung dieses Geraets.  Das ist "
                   "eine ausdrueckliche Angabe, kein Vergleich -- sie gilt "
                   "auch dann, wenn der Treiber keine Bereiche belegt.\r\n\r\n"
                   "Den Wert setzt \"Treiber einrichten\".");
            break;
        case ZU_GENAU:
            scat(t,"Der Treiber hat in HKLM\\HARDWARE\\RESOURCEMAP einen "
                   "Bereich eingetragen, dessen Anfangsadresse und "
                   "Busnummer genau mit einer Basisadresse dieses Geraets "
                   "uebereinstimmen.\r\n\r\n"
                   "Das ist eindeutig, solange keine zwei Geraete denselben "
                   "Bereich belegen -- was der Bus nicht zulaesst.");
            break;
        case ZU_ADRESSE:
            scat(t,"Die Anfangsadresse stimmt ueberein, die Busnummer "
                   "nicht.  Manche Treiber tragen ihre Bereiche mit "
                   "Busnummer 0 ein, gleichgueltig auf welchem Bus das "
                   "Geraet sitzt.\r\n\r\n"
                   "Wahrscheinlich richtig, aber schwaecher belegt als der "
                   "genaue Fall.");
            break;
        default:
            scat(t,"Kein Treiber zugeordnet.\r\n\r\n"
                   "Moeglich ist dreierlei: es ist keiner installiert; er "
                   "ist installiert und geladen, traegt aber keine Bereiche "
                   "in die ResourceMap ein; oder er traegt sie unter einer "
                   "Adresse ein, die zu keiner Basisadresse dieses Geraets "
                   "passt.\r\n\r\n"
                   "Die Schaltflaeche \"Diagnose\" schreibt alles auf, was "
                   "gelesen wurde -- damit laesst sich der Fall "
                   "unterscheiden.");
            break;
        }
        AddText(hwnd,y,110,t);
        return TRUE;
    }
    return FALSE;
}

static BOOL CALLBACK PageRessourcen(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if(msg==WM_INITDIALOG){
        PCI_ENTRY* e=&gDev[gSel];
        static char t[1024];
        int b, n=0;

        t[0]=0;
        scat(t,"Typ        Anfang     Basisadresse\r\n");
        scat(t,"--------------------------------------\r\n");

        for(b=0;b<6;b++){
            DWORD bar=e->Bar[b];
            if(!bar) continue;
            n++;
            if(bar&1){ scat(t,"Port       "); cathex(t,bar&0xFFFFFFFC,4); scat(t,"       "); }
            else     { scat(t,"Speicher   "); cathex(t,bar&0xFFFFFFF0,8); scat(t,"   "); }
            scat(t,"BAR"); catnum(t,b);
            scat(t,"\r\n");
        }

        if(!n) scat(t,"(keine)\r\n");

        scat(t,"\r\nUnterbrechung\r\n    ");
        if(e->InterruptLine && e->InterruptLine!=0xFF){
            catnum(t,e->InterruptLine);
            scat(t,"   -- Angabe des BIOS, unverbindlich.\r\n"
                   "    Mehrere Geraete melden hier denselben Wert;\r\n"
                   "    die tatsaechliche Zuordnung macht die\r\n"
                   "    PCI-Routingtabelle.");
        } else {
            scat(t,"keine angegeben");
        }

        AddText(hwnd,12,200,t);
        return TRUE;
    }
    return FALSE;
}

/*
    Absturzstelle des ersten Entwurfs.

    PSH_PROPSHEETPAGE sagt der Systembibliothek, dass phpage/ppsp auf
    ein Feld von PROPSHEETPAGE-Strukturen zeigt.  Uebergeben wurde
    aber das Feld der drei Handles aus CreatePropertySheetPage.
    comctl32 las die drei Handles als dwSize, dwFlags und hInstance
    der ersten Seite und den Rest -- pResource, pfnDlgProc -- aus dem
    Stapel dahinter.  Der Sprung auf diesen Zufallszeiger ist der
    Fehler in comctl32, den man von aussen als 0x71xxxxxx sieht.

    Zwei Wege waeren richtig: Handles ohne das Flag, oder die
    Strukturen mit dem Flag.  Der zweite ist der kuerzere -- dann
    entfaellt CreatePropertySheetPage ganz, denn PropertySheet legt
    die Seiten selbst an und kopiert die Strukturen beim Eintritt.
*/

/*
    NT 4.0 kennt nur die erste Fassung dieser beiden Strukturen.  Der
    NT-4.0-Kopfbaum liefert genau die, also stimmt dort sizeof.
    Neuere Kopfdateien haengen Felder an und stellen dafuer die
    _V1_SIZE-Makros bereit -- wer dann sizeof nimmt, bekommt von
    comctl32 eine Abfuhr, weil es die groessere Fassung nicht kennt.
*/

#ifndef PROPSHEETPAGEA_V1_SIZE
#define PROPSHEETPAGEA_V1_SIZE      sizeof(PROPSHEETPAGEA)
#endif
#ifndef PROPSHEETHEADERA_V1_SIZE
#define PROPSHEETHEADERA_V1_SIZE    sizeof(PROPSHEETHEADERA)
#endif

/*  comctl32 geht Seite fuer Seite um dwSize weiter.  dwSize ist die
    alte V1-Groesse (die kennt die comctl32 von NT 4.0), das Feld
    PROPSHEETPAGEA ist mit neueren Kopfdateien aber groesser -- ab der
    zweiten Seite las comctl32 mitten in der ersten, und die Reiter
    "Treiber"/"Ressourcen" bzw. S(T_SCHNITTSTELLEN) fehlten.  Deshalb die
    Seiten dicht mit V1-Abstand in einen Puffer legen.  */
static BYTE gSeiten[4*PROPSHEETPAGEA_V1_SIZE];

static LPCPROPSHEETPAGEA SeitenDicht(const PROPSHEETPAGEA* psp, int n)
{
    int i;
    for(i=0;i<n && i<4;i++) memcpy(gSeiten+i*PROPSHEETPAGEA_V1_SIZE,&psp[i],PROPSHEETPAGEA_V1_SIZE);
    return (LPCPROPSHEETPAGEA)gSeiten;
}

/* ---------------- Eigenschaften eines USB-Geraets ---------------- */

static BOOL CALLBACK PageUsbAllgemein(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if(msg==WM_INITDIALOG && gUsbSel>=0){
        const NTU_USB_GERAET* g=&gUsb.Geraet[gUsbSel];
        char t[200]; int y=12;

        t[0]=0; UsbName(g,t,sizeof(t));
        AddLine(hwnd,&y,S(T_BEZEICHNUNG),t);
        AddLine(hwnd,&y,S(T_HERSTELLER), g->Hersteller[0] ? g->Hersteller : S(T_KEIN_TEXT));
        AddLine(hwnd,&y,S(T_PRODUKT),    g->Produkt[0] ? g->Produkt : S(T_KEIN_TEXT));
        if(g->Seriennummer[0]) AddLine(hwnd,&y,S(T_SERIENNUMMER),g->Seriennummer);
        t[0]=0; scat(t,"USB\\VID_"); cathex(t,g->Vendor,4); scat(t,"&PID_"); cathex(t,g->Product,4);
        scat(t,"  (Fassung "); catbcd(t,g->Geraetefassung); scat(t,")");
        AddLine(hwnd,&y,S(T_KENNUNG),t);
        t[0]=0; scat(t,UsbKlasse(g->Klasse,g->Unterklasse,g->Protokoll));
        scat(t,"  ("); cathex(t,g->Klasse,2); scat(t,"/"); cathex(t,g->Unterklasse,2);
        scat(t,"/"); cathex(t,g->Protokoll,2); scat(t,")");
        AddLine(hwnd,&y,S(T_GERAETEKLASSE),t);
        t[0]=0; scat(t,"USB "); catbcd(t,g->UsbFassung); scat(t,", "); scat(t,UsbTempo(g->Geschwindigkeit));
        AddLine(hwnd,&y,S(T_ANSCHLUSS),t);
        t[0]=0; scat(t,"Bus "); catnum(t,g->Bus+1); scat(t,", Adresse "); catnum(t,g->Adresse);
        if(g->Tiefe>0){ scat(t,", Hub-Anschluss "); catnum(t,g->Anschluss);
                        scat(t," (Tiefe "); catnum(t,g->Tiefe); scat(t,")"); }
        AddLine(hwnd,&y,S(T_ORT),t);
        if(g->StromMa){ t[0]=0; catnum(t,g->StromMa); scat(t," mA");
                        AddLine(hwnd,&y,S(T_STROMBEDARF),t); }
        t[0]=0; catnum(t,g->Konfigurationen); scat(t," Konfiguration(en), Endpunkt 0: ");
        catnum(t,g->MaxPaket0); scat(t," Byte");
        AddLine(hwnd,&y,S(T_AUFBAU),t);
        if(UsbOhneTreiber(g))
            AddText(hwnd,y+6,40,"Kein Treiber haengt an diesem Geraet.  Der USB-Treiber stellt "
                    "es roh bereit (\\\\.\\UsbDevN) -- ein Programm kann es so ansprechen.");
        return TRUE;
    }
    return FALSE;
}

static BOOL CALLBACK PageUsbSchnittstellen(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if(msg==WM_INITDIALOG && gUsbSel>=0){
        const NTU_USB_GERAET* g=&gUsb.Geraet[gUsbSel];
        char l[48], t[200]; int y=12, i;
        if(g->AnzahlIf==0) AddLine(hwnd,&y,"","keine Schnittstellen gemeldet");
        for(i=0;i<g->AnzahlIf;i++){
            const NTU_USB_IF* x=&g->If[i];
            l[0]=0; scat(l,"Schnittstelle "); catnum(l,x->Nummer); scat(l,":");
            t[0]=0; scat(t,UsbKlasse(x->Klasse,x->Unterklasse,x->Protokoll));
            scat(t," ("); cathex(t,x->Klasse,2); scat(t,"/"); cathex(t,x->Unterklasse,2);
            scat(t,"/"); cathex(t,x->Protokoll,2); scat(t,"), Treiber: ");
            scat(t, x->Treiber[0] ? x->Treiber : S(T_KEIN_TREIBER));
            AddLine(hwnd,&y,l,t);
        }
        AddText(hwnd,y+8,40,"Treibernamen wie im FreeBSD-USB-Stapel: ukbd = Tastatur, "
                "ums = Maus, umass = Massenspeicher, ntujoy = Gamepad, ntugen = roh.");
        return TRUE;
    }
    return FALSE;
}

static void ShowUsbProperties(HWND parent, int idx)
{
    PROPSHEETPAGEA psp[2];
    PROPSHEETHEADERA psh;
    char titel[256];
    int i;

    if(!gUsbOk || idx<0 || (DWORD)idx>=gUsb.Anzahl) return;
    gUsbSel=idx;
    memset(psp,0,sizeof(psp));
    for(i=0;i<2;i++){
        psp[i].dwSize      = PROPSHEETPAGEA_V1_SIZE;
        psp[i].dwFlags     = PSP_DEFAULT | PSP_USETITLE | PSP_DLGINDIRECT;
        psp[i].hInstance   = gInst;
        psp[i].pResource   = (LPCDLGTEMPLATEA)&gTpl;
    }
    psp[0].pszTitle=S(T_ALLGEMEIN);       psp[0].pfnDlgProc=(DLGPROC)PageUsbAllgemein;
    psp[1].pszTitle=S(T_SCHNITTSTELLEN);  psp[1].pfnDlgProc=(DLGPROC)PageUsbSchnittstellen;
    titel[0]=0; UsbName(&gUsb.Geraet[idx],titel,200); scat(titel," Eigenschaften");
    memset(&psh,0,sizeof(psh));
    psh.dwSize      = PROPSHEETHEADERA_V1_SIZE;
    psh.dwFlags     = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW;
    psh.hwndParent  = parent;
    psh.hInstance   = gInst;
    psh.pszCaption  = titel;
    psh.nPages      = 2;
    psh.ppsp        = SeitenDicht(psp,2);
    PropertySheetA(&psh);
}

static void ShowProperties(HWND parent)
{
    PROPSHEETPAGEA psp[3];
    PROPSHEETHEADERA psh;
    char titel[256];
    int i;

    if(gSel>=USB_BASIS){ ShowUsbProperties(parent,gSel-USB_BASIS); return; }
    if(gSel<0 || (DWORD)gSel>=gDevCount) return;

    memset(psp,0,sizeof(psp));
    for(i=0;i<3;i++){
        psp[i].dwSize      = PROPSHEETPAGEA_V1_SIZE;
        psp[i].dwFlags     = PSP_DEFAULT | PSP_USETITLE | PSP_DLGINDIRECT;
        psp[i].hInstance   = gInst;
        psp[i].pResource   = (LPCDLGTEMPLATEA)&gTpl;
    }
    psp[0].pszTitle=S(T_ALLGEMEIN);  psp[0].pfnDlgProc=(DLGPROC)PageAllgemein;
    psp[1].pszTitle="Treiber";    psp[1].pfnDlgProc=(DLGPROC)PageTreiber;
    psp[2].pszTitle="Ressourcen"; psp[2].pfnDlgProc=(DLGPROC)PageRessourcen;

    titel[0]=0;
    { const char* h=VendorName(gDev[gSel].VendorID);
      const char* g=DeviceName(gDev[gSel].VendorID,gDev[gSel].DeviceID);
      if(h[0]){ scatlim(titel,h,48); scat(titel," "); }
      if(g[0]){
          scatlim(titel,g,140);
      } else {
          cathex(titel,gDev[gSel].VendorID,4);
          scat(titel,":");
          cathex(titel,gDev[gSel].DeviceID,4);
      }
    }
    scat(titel," Eigenschaften");

    memset(&psh,0,sizeof(psh));
    psh.dwSize      = PROPSHEETHEADERA_V1_SIZE;
    psh.dwFlags     = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW;
    psh.hwndParent  = parent;
    psh.hInstance   = gInst;
    psh.pszCaption  = titel;
    psh.nPages      = 3;
    psh.ppsp        = SeitenDicht(psp,3);

    PropertySheetA(&psh);
}

/* ---------------- Diagnose ---------------- */

/*
    Schreibt alles auf, was gelesen wurde: jedes Geraet mit seinen
    Basisadressen, jeden Bereich aus der ResourceMap mit Treibername,
    Bus und Anfang, und jeden Dienst, der einen DevMgrGeraet-Vermerk
    traegt.  Damit laesst sich die Frage "der Treiber ist doch
    geladen, warum steht er nicht da" beantworten, statt sie zu
    vermuten -- man sieht unmittelbar, ob der Treiber ueberhaupt
    einen Bereich eingetragen hat und ob dessen Anfang zu einer
    Basisadresse passt.
*/

static char  gDump[65536];
static DWORD gDumpLen;

static void DAdd(const char* s)
{
    DWORD i=0;
    while(s[i] && gDumpLen < sizeof(gDump)-2) gDump[gDumpLen++]=s[i++];
    gDump[gDumpLen]=0;
}

static void DCol(const char* s, int breite)
{
    int n=slen(s);
    DAdd(s);
    while(n<breite && gDumpLen<sizeof(gDump)-2){ gDump[gDumpLen++]=' '; n++; }
    gDump[gDumpLen]=0;
}

static void DHex(DWORD v, int n){ char t[16]; t[0]=0; cathex(t,v,n); DAdd(t); }
static void DNum(DWORD v){ char t[24]; t[0]=0; catnum(t,v); DAdd(t); }

static void DumpDienste(void)
{
    HKEY root, k;
    DWORD idx=0, gefunden=0;

    DAdd("DIENSTE MIT DevMgrGeraet-VERMERK\r\n");
    DAdd("Dienst          Kennung                        Start                  Zustand\r\n");
    DAdd("-----------------------------------------------------------------------------\r\n");

    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,SVCROOT,0,
                     KEY_ENUMERATE_SUB_KEYS,&root)!=ERROR_SUCCESS){
        DAdd("(Services-Schluessel nicht lesbar)\r\n");
        return;
    }
    for(;;){
        char name[256], path[512], wert[160];
        DWORD nlen=sizeof(name), wlen, typ=0;

        if(RegEnumKeyExA(root,idx,name,&nlen,NULL,NULL,NULL,NULL)!=ERROR_SUCCESS) break;
        idx++;

        scpy(path,SVCROOT); scat(path,"\\"); scatlim(path,name,200);
        if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,path,0,KEY_QUERY_VALUE,&k)!=ERROR_SUCCESS)
            continue;

        wlen=sizeof(wert)-1;
        if(RegQueryValueExA(k,"DevMgrGeraet",NULL,&typ,(LPBYTE)wert,&wlen)==ERROR_SUCCESS
           && typ==REG_SZ){
            wert[sizeof(wert)-1]=0;
            DCol(name,16);
            DCol(wert,31);
            DCol(StartName(DienstStart(name)),23);
            DAdd(ZustandName(DienstZustand(name)));
            DAdd("\r\n");
            gefunden++;
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);

    if(!gefunden)
        DAdd("(keiner -- kein Treiber wurde ueber \"Treiber einrichten\" angelegt)\r\n");
}

/*  USB-Geraete im Stil von lsusb.  */
static void DumpUsb(void)
{
    DWORD i; int k; char z[200];
    DAdd("\r\nUSB-GERAETE");
    if(!gUsbOk){ DAdd(" -- USB-Treiber (usbnt) nicht geladen\r\n"); return; }
    DAdd(" (ueber "); DAdd(gUsbQuelle); DAdd(")\r\n");
    for(i=0;i<gUsb.Anzahl;i++){
        const NTU_USB_GERAET* g=&gUsb.Geraet[i];
        z[0]=0; scat(z,"Bus "); if(g->Bus+1<100) scat(z,"0"); if(g->Bus+1<10) scat(z,"0"); catnum(z,g->Bus+1);
        scat(z," Device "); if(g->Adresse<100) scat(z,"0"); if(g->Adresse<10) scat(z,"0"); catnum(z,g->Adresse);
        scat(z,": ID "); cathex(z,g->Vendor,4); scat(z,":"); cathex(z,g->Product,4); scat(z," ");
        DAdd(z); z[0]=0; UsbName(g,z,sizeof(z)); DAdd(z); DAdd("\r\n");
        z[0]=0; scat(z,"    "); scat(z,UsbTempo(g->Geschwindigkeit)); scat(z,", Klasse ");
        scat(z,UsbKlasse(g->Klasse,g->Unterklasse,g->Protokoll));
        if(g->Tiefe){ scat(z,", Hub-Anschluss "); catnum(z,g->Anschluss); scat(z," an Adresse "); catnum(z,g->ElternAdresse); }
        DAdd(z); DAdd("\r\n");
        for(k=0;k<g->AnzahlIf;k++){
            z[0]=0; scat(z,"    If "); catnum(z,g->If[k].Nummer); scat(z,": ");
            scat(z,UsbKlasse(g->If[k].Klasse,g->If[k].Unterklasse,g->If[k].Protokoll));
            scat(z," -> "); scat(z,g->If[k].Treiber[0]?g->If[k].Treiber:S(T_KEIN_TREIBER));
            DAdd(z); DAdd("\r\n");
        }
    }
}

static void DumpDiagnose(HWND parent)
{
    char pfad[MAX_PATH+32], t[200];
    DWORD i, b, n;
    HANDLE h;
    DWORD geschrieben;

    gDumpLen=0; gDump[0]=0;

    DAdd("Geraetemanager -- Diagnose\r\n");
    DAdd("==========================\r\n\r\n");

    n=sizeof(t);
    if(GetComputerNameA(t,&n)){ DAdd("Rechner    "); DAdd(t); DAdd("\r\n"); }

    DAdd("PCI.IDS    ");
    if(PciIdsLoaded()){
        DAdd(PciIdsSource()); DAdd(", "); DNum(PciIdsVendorCount()); DAdd(" Hersteller");
    } else DAdd("nicht gefunden");
    DAdd("\r\n");

    DAdd("Geraete    "); DNum(gDevCount); DAdd("\r\n");
    DAdd("Bereiche   "); DNum(gClaimCount); DAdd("\r\n\r\n");

    /* --- Geraete --- */

    DAdd("GERAETE\r\n");
    DAdd("Ort     Ven  Dev  Kl/Su Rev  Basisadressen                 "
         "Dienst          Quelle\r\n");
    DAdd("--------------------------------------------------------------"
         "-----------------------------------\r\n");

    for(i=0;i<gDevCount;i++){
        t[0]=0;
        cathex(t,gDev[i].Bus,2); scat(t,":");
        cathex(t,gDev[i].Device,2); scat(t,".");
        catnum(t,gDev[i].Function);
        DCol(t,8);

        DHex(gDev[i].VendorID,4); DAdd(" ");
        DHex(gDev[i].DeviceID,4); DAdd(" ");
        DHex(gDev[i].BaseClass,2); DAdd("/");
        DHex(gDev[i].SubClass,2);  DAdd("  ");
        DHex(gDev[i].RevisionID,2); DAdd("   ");

        t[0]=0;
        for(b=0;b<6;b++){
            DWORD bar=gDev[i].Bar[b];
            if(!bar) continue;
            if(bar&1){ scat(t,"p"); cathex(t,bar&0xFFFFFFFC,4); }
            else     { scat(t,"m"); cathex(t,bar&0xFFFFFFF0,8); }
            scat(t," ");
        }
        if(!t[0]) scpy(t,"-");
        DCol(t,30);

        DCol(gSvc[i][0]?gSvc[i]:"-",16);
        DAdd(HerkunftName(gSvcHow[i]));
        DAdd("\r\n");
    }

    DAdd("\r\np = Portbereich, m = Speicherbereich\r\n\r\n");

    /* --- ResourceMap --- */

    DAdd("BELEGTE BEREICHE AUS HKLM\\HARDWARE\\RESOURCEMAP\r\n");
    DAdd("Treiber                    Bus  Typ       Anfang\r\n");
    DAdd("-------------------------------------------------\r\n");

    if(!gClaimCount)
        DAdd("(keine -- kein Treiber hat Bereiche eingetragen)\r\n");

    for(i=0;i<gClaimCount;i++){
        DCol(gClaim[i].Service,27);
        t[0]=0; catnum(t,gClaim[i].Bus);
        DCol(t,5);
        DCol(gClaim[i].Type==1?"Port":"Speicher",10);
        DHex(gClaim[i].Start,8);
        DAdd("\r\n");
    }

    DAdd("\r\n");
    DumpDienste();

    DAdd("\r\nHINWEIS\r\n");
    DAdd("Steht bei einem Geraet kein Dienst, dann pruefen: taucht der "
         "Treiber\r\noben unter den Bereichen ueberhaupt auf?  Wenn nein, "
         "traegt er nichts\r\nein und laesst sich nur ueber den "
         "DevMgrGeraet-Vermerk zuordnen.  Wenn ja,\r\nvergleichen Sie seinen "
         "Anfang mit den Basisadressen des Geraets.\r\n");

    /* --- schreiben --- */

    n=GetSystemDirectoryA(pfad,MAX_PATH-20);
    if(!n) return;
    if(pfad[n-1]!='\\'){ pfad[n]='\\'; pfad[n+1]=0; }
    scat(pfad,"DEVMGR.TXT");

    h=CreateFileA(pfad,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(h==INVALID_HANDLE_VALUE){
        MessageBoxA(parent,"Die Diagnosedatei liess sich nicht anlegen.",
                    "Diagnose",MB_OK|MB_ICONSTOP);
        return;
    }
    WriteFile(h,gDump,gDumpLen,&geschrieben,NULL);
    FlushFileBuffers(h);
    CloseHandle(h);

    t[0]=0;
    scat(t,"Geschrieben:\r\n\r\n");
    scatlim(t,pfad,180);
    MessageBoxA(parent,t,"Diagnose",MB_OK|MB_ICONINFORMATION);
}

/* ---------------- Treiber einrichten ---------------- */

/*
    Ein Treiber wird hier nur *eingetragen*, nie in den Startablauf
    gehaengt: Start = 3, SERVICE_DEMAND_START, also manuell.  Wer ihn
    geladen haben will, laedt ihn danach von Hand -- gleich aus der
    Rueckfrage oder spaeter mit "net start <Name>".

    Der Grund ist die Rueckholbarkeit.  Ein Treiber mit Start = 0 oder
    1, der beim Laden bugcheckt, nimmt das System mit und laesst sich
    ohne zweite Installation nicht mehr abschalten -- man braucht die
    Wiederherstellungskonsole oder eine zweite Installation, um den
    Eintrag wieder herauszunehmen.  Bei "manuell" bootet die Maschine
    in jedem Fall wieder hoch, und der Eintrag laesst sich in Ruhe
    aendern.

    Wer einen Treiber wirklich beim Start geladen haben will, setzt
    Start von Hand herunter -- nachdem er einmal von Hand geladen hat
    und weiss, dass es gutgeht.
*/

#ifndef OPENFILENAME_SIZE_VERSION_400
#define OPENFILENAME_SIZE_VERSION_400   sizeof(OPENFILENAMEA)
#endif

#define SVCKEY "SYSTEM\\CurrentControlSet\\Services\\"

/*
    Der Dienstname wird aus dem Dateinamen ohne Erweiterung gebildet.
    Das ist die Gepflogenheit -- ATAPI.SYS heisst als Dienst "atapi".
*/

static BOOL DienstnameAus(const char* dateiname, char* out)
{
    int i=0;
    while(dateiname[i] && dateiname[i]!='.' && i<31){
        char c=dateiname[i];
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))
            return FALSE;
        out[i]=c; i++;
    }
    out[i]=0;
    return i>0;
}

/*
    Notiert am Dienstschluessel, fuer welches Geraet der Treiber
    gedacht war.  Der Dienstverwalter sieht ueber unbekannte Werte
    hinweg; es aendert am Verhalten nichts und hilft spaeter beim
    Wiedererkennen.
*/

static void MerkeGeraet(const char* dienst)
{
    char path[320], id[96];
    HKEY k; DWORD disp;

    if(gSel<0 || (DWORD)gSel>=gDevCount) return;

    scpy(path,SVCKEY); scat(path,dienst);
    if(RegCreateKeyExA(HKEY_LOCAL_MACHINE,path,0,NULL,REG_OPTION_NON_VOLATILE,
                       KEY_SET_VALUE,NULL,&k,&disp)!=ERROR_SUCCESS) return;

    id[0]=0;
    scat(id,"PCI\\VEN_"); cathex(id,gDev[gSel].VendorID,4);
    scat(id,"&DEV_");    cathex(id,gDev[gSel].DeviceID,4);
    if(gDev[gSel].SubVendorID||gDev[gSel].SubSystemID){
        scat(id,"&SUBSYS_");
        cathex(id,gDev[gSel].SubSystemID,4);
        cathex(id,gDev[gSel].SubVendorID,4);
    }
    RegSetValueExA(k,"DevMgrGeraet",0,REG_SZ,(const BYTE*)id,(DWORD)slen(id)+1);
    RegCloseKey(k);
}

static void Sagen(HWND p, const char* text, UINT art)
{
    MessageBoxA(p,text,"Treiber einrichten",MB_OK|art);
}

/* ---------------- PCI.IDS aktualisieren ---------------- */

/*  Damit man ohne Nachsehen erkennt, woher die Namen kommen.  */
static void TitelSetzen(HWND hwnd)
{
    char cap[200], stand[40];
    scpy(cap,S(T_TITEL)); scat(cap,"  --  ");
    if(PciIdsLoaded()){
        scat(cap,"PCI.IDS");
        PciIdsStand(stand,sizeof(stand));
        if(stand[0]){ scat(cap," vom "); scat(cap,stand); }
        scat(cap,", "); catnum(cap,PciIdsVendorCount()); scat(cap," Hersteller");
    } else {
        scat(cap,"PCI.IDS nicht gefunden, nur eingebaute Namen");
    }
    SetWindowTextA(hwnd,cap);
}

#define GM_SCHLUESSEL "SOFTWARE\\Geraetemanager"
#define GM_URL_VORGABE "https://pci-ids.ucw.cz/v2.2/pci.ids"

static void PciIdsUrl(char* url, DWORD max)
{
    HKEY k; DWORD typ, n=max;
    scpy(url,GM_URL_VORGABE);
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,GM_SCHLUESSEL,0,KEY_READ,&k)==ERROR_SUCCESS){
        if(RegQueryValueExA(k,"PciIdsUrl",NULL,&typ,(BYTE*)url,&n)!=ERROR_SUCCESS || typ!=REG_SZ || !url[0])
            scpy(url,GM_URL_VORGABE);
        RegCloseKey(k);
    }
}

static HWND gLadeFenster;
static HWND gIdsHaupt;          /* Hauptfenster (gMain setzt das Original nirgends) */
static void LadeFortschritt(DWORD n)
{
    char t[80]; MSG m;
    if(!gLadeFenster) return;
    scpy(t,"Geraetemanager  --  lade PCI.IDS: "); catnum(t,n/1024); scat(t," KB");
    SetWindowTextA(gLadeFenster,t);
    while(PeekMessageA(&m,NULL,WM_PAINT,WM_PAINT,PM_REMOVE)) DispatchMessageA(&m);
}

/*  HTTPS ueber PCIHOLE.EXE (wolfSSL, TLS 1.2, prueft Zertifikatskette
    gegen CACERT.PEM und den Rechnernamen).  Eigenes Programm, damit der
    Geraetemanager klein und ohne C-Laufzeit bleibt.  Ausgabe zeilen-
    weise: "KB n", "OK n", "FEHLER text".  Gesucht neben dem Programm,
    dann in system32.  */
static BOOL PciholeFinden(char* pfad)
{
    char d[MAX_PATH+16]; DWORD n; int i;
    n=GetModuleFileNameA(gInst,d,MAX_PATH);
    if(n && n<MAX_PATH){
        for(i=(int)n-1;i>=0 && d[i]!='\\';i--){}
        d[i+1]=0; scat(d,"PCIHOLE.EXE");
        if(GetFileAttributesA(d)!=0xFFFFFFFF){ scpy(pfad,d); return TRUE; }
    }
    n=GetSystemDirectoryA(d,MAX_PATH);
    if(n && n<MAX_PATH){
        if(d[n-1]!='\\') scat(d,"\\");
        scat(d,"PCIHOLE.EXE");
        if(GetFileAttributesA(d)!=0xFFFFFFFF){ scpy(pfad,d); return TRUE; }
    }
    return FALSE;
}

#ifndef NUR_ANSEHEN
static BOOL PciholeHolen(const char* prog, const char* url, BYTE** daten, DWORD* laenge, char* fehler, int fm)
{
    static char zeile[1400], ziel[MAX_PATH+16], rest[1200];
    SECURITY_ATTRIBUTES sa; HANDLE lesen, schreiben, h;
    STARTUPINFOA si; PROCESS_INFORMATION pi; DWORD n, rn=0, code=1, groesse, gelesen=0;
    char puffer[512]; BOOL ok=FALSE; int i;

    *daten=NULL; *laenge=0; fehler[0]=0;
    n=GetTempPathA(MAX_PATH,ziel); if(!n || n>=MAX_PATH) GetSystemDirectoryA(ziel,MAX_PATH);
    if(ziel[slen(ziel)-1]!='\\') scat(ziel,"\\");
    scat(ziel,"PCIIDS.TMP"); DeleteFileA(ziel);

    memset(&sa,0,sizeof(sa)); sa.nLength=sizeof(sa); sa.bInheritHandle=TRUE;
    if(!CreatePipe(&lesen,&schreiben,&sa,0)){ scpy(fehler,"Pipe fehlgeschlagen"); return FALSE; }
    memset(&si,0,sizeof(si)); si.cb=sizeof(si);
    si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW; si.wShowWindow=SW_HIDE;
    si.hStdOutput=schreiben; si.hStdError=schreiben; si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    zeile[0]=0; scat(zeile,"\""); scatlim(zeile,prog,MAX_PATH); scat(zeile,"\" \"");
    scatlim(zeile,url,600); scat(zeile,"\" \""); scat(zeile,ziel); scat(zeile,"\"");
    if(!CreateProcessA(NULL,zeile,NULL,NULL,TRUE,0,NULL,NULL,&si,&pi)){
        CloseHandle(lesen); CloseHandle(schreiben);
        scpy(fehler,"PCIHOLE.EXE liess sich nicht starten."); return FALSE;
    }
    CloseHandle(schreiben);
    rest[0]=0;
    while(ReadFile(lesen,puffer,sizeof(puffer)-1,&n,NULL) && n>0){
        DWORD k;
        for(k=0;k<n;k++){
            char c=puffer[k];
            if(c=='\r') continue;
            if(c=='\n'){
                rest[rn]=0; rn=0;
                if(rest[0]=='K'&&rest[1]=='B'&&rest[2]==' '){
                    DWORD kb=0; for(i=3;rest[i]>='0'&&rest[i]<='9';i++) kb=kb*10+(DWORD)(rest[i]-'0');
                    LadeFortschritt(kb*1024);
                } else if(rest[0]=='F'&&rest[1]=='E'&&rest[2]=='H'){
                    scpy(fehler,""); scatlim(fehler,rest+7,fm-2);
                }
                continue;
            }
            if(rn<sizeof(rest)-1) rest[rn++]=c;
        }
    }
    CloseHandle(lesen);
    WaitForSingleObject(pi.hProcess,120000);
    GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if(code!=0){ if(!fehler[0]) scpy(fehler,"PCIHOLE.EXE meldet einen Fehler."); DeleteFileA(ziel); return FALSE; }

    h=CreateFileA(ziel,GENERIC_READ,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(h==INVALID_HANDLE_VALUE){ scpy(fehler,"PCIHOLE.EXE hat keine Datei geschrieben."); return FALSE; }
    groesse=GetFileSize(h,NULL);
    if(groesse!=0xFFFFFFFF && groesse<=DL_MAX){
        *daten=(BYTE*)dl_neu(groesse+1);
        if(*daten && ReadFile(h,*daten,groesse,&gelesen,NULL) && gelesen==groesse) ok=TRUE;
    }
    CloseHandle(h); DeleteFileA(ziel);
    if(!ok){ dl_weg(*daten); *daten=NULL; scpy(fehler,"Geladene Datei unlesbar."); return FALSE; }
    if(!DlPruefe(*daten,groesse)){
        dl_weg(*daten); *daten=NULL;
        scpy(fehler,"Die geladene Datei ist keine PCI.IDS (zu klein oder falscher Inhalt).  Die vorhandene bleibt.");
        return FALSE;
    }
    *laenge=groesse;
    return TRUE;
}
#endif /* NUR_ANSEHEN */

/*  Neue Datei dorthin, wo die benutzte liegt (sonst system32):
    erst als .NEU schreiben, die alte zu .ALT, dann umbenennen.  */
static BOOL PciIdsSchreiben(HWND parent, const BYTE* b, DWORD n)
{
    static char ziel[MAX_PATH+16], neu[MAX_PATH+16], alt[MAX_PATH+16], t[700], vorher[40], nachher[40];
    HANDLE h; DWORD w=0, k;
    const char* q=PciIdsSource();

    if(q) scpy(ziel,q);
    else {
        k=GetSystemDirectoryA(ziel,MAX_PATH);
        if(!k) return FALSE;
        if(ziel[k-1]!='\\') scat(ziel,"\\");
        scat(ziel,"PCI.IDS");
    }
    scpy(neu,ziel); scat(neu,".NEU");
    scpy(alt,ziel); scat(alt,".ALT");
    h=CreateFileA(neu,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(h==INVALID_HANDLE_VALUE){
        scpy(t,"Die Datei liess sich nicht anlegen:\r\n\r\n"); scatlim(t,neu,300);
        scat(t,"\r\n\r\nFehlen Administratorrechte?");
        MessageBoxA(parent,t,"PCI.IDS",MB_OK|MB_ICONSTOP); return FALSE;
    }
    if(!WriteFile(h,b,n,&w,NULL) || w!=n){ CloseHandle(h); DeleteFileA(neu);
        MessageBoxA(parent,"Schreiben fehlgeschlagen (Platte voll?).","PCI.IDS",MB_OK|MB_ICONSTOP); return FALSE; }
    FlushFileBuffers(h); CloseHandle(h);

    PciIdsStand(vorher,sizeof(vorher));
    if(GetFileAttributesA(ziel)!=0xFFFFFFFF){
        DeleteFileA(alt);
        if(!MoveFileA(ziel,alt)){ DeleteFileA(neu);
            MessageBoxA(parent,"Die alte PCI.IDS liess sich nicht umbenennen.","PCI.IDS",MB_OK|MB_ICONSTOP); return FALSE; }
    }
    if(!MoveFileA(neu,ziel)){
        MoveFileA(alt,ziel);
        MessageBoxA(parent,"Die neue PCI.IDS liess sich nicht einsetzen; die alte bleibt.","PCI.IDS",MB_OK|MB_ICONSTOP);
        return FALSE;
    }

    PciIdsNeuLaden();
    Match();
    BuildTree();
    TitelSetzen(gIdsHaupt);

    PciIdsStand(nachher,sizeof(nachher));
    scpy(t,"PCI.IDS aktualisiert.\r\n\r\n");
    if(vorher[0]){ scat(t,"Bisher:  "); scat(t,vorher); scat(t,"\r\n"); }
    scat(t,"Jetzt:   "); scat(t,nachher[0]?nachher:"(ohne Standangabe)");
    scat(t,", "); catnum(t,PciIdsVendorCount()); scat(t," Hersteller\r\n\r\n");
    scatlim(t,ziel,300);
    if(GetFileAttributesA(alt)!=0xFFFFFFFF){ scat(t,"\r\n(die vorige liegt als "); scatlim(t,alt,250); scat(t,")"); }
    MessageBoxA(parent,t,"PCI.IDS",MB_OK|MB_ICONINFORMATION);
    return TRUE;
}

static void PciIdsFenster(HWND parent)
{
    static char url[512], t[1500], stand[40], fehler[700];
    const char* q=PciIdsSource();
    int wahl;

    gIdsHaupt=parent;
    PciIdsUrl(url,sizeof(url));
    scpy(t,"Vorhandene PCI.IDS:  ");
    if(q){ scatlim(t,q,260); PciIdsStand(stand,sizeof(stand));
           scat(t,"\r\nStand:  "); scat(t,stand[0]?stand:S(T_UNBEKANNT));
           scat(t,", "); catnum(t,PciIdsVendorCount()); scat(t," Hersteller"); }
    else scat(t,"keine -- es gelten nur die eingebauten Namen");
    scat(t,"\r\n\r\nAktualisieren?\r\n\r\n"
           "Ja  =  aus dem Internet laden:\r\n      ");
    scatlim(t,url,300);
    scat(t,"\r\n      (https:// ueber PCIHOLE.EXE mit TLS 1.2 und Zertifikatspruefung;\r\n"
           "       fehlt es, wird http:// versucht)\r\n\r\n"
           "Nein  =  aus einer Datei uebernehmen\r\n"
           "      (z. B. auf einem anderen Rechner geladen)\r\n\r\n"
           "Abbrechen  =  nichts aendern");
    wahl=MessageBoxA(parent,t,"PCI.IDS",MB_YESNOCANCEL|MB_ICONQUESTION|MB_DEFBUTTON3);

    if(wahl==IDYES){
        BYTE* b; DWORD n; HCURSOR alt; BOOL gut; static char prog[MAX_PATH+16];
        EnableWindow(parent,FALSE);
        alt=SetCursor(LoadCursorA(NULL,(LPCSTR)IDC_WAIT));
        gLadeFenster=gIdsHaupt;
        if((url[0]=='h'||url[0]=='H') && (url[4]=='s'||url[4]=='S')){
#ifdef NUR_ANSEHEN
            /*  Schlanke CPL: kein PCIHOLE-Start (kein CreateProcess).  https://
                wird als http:// versucht; fuer echtes HTTPS die EXE-Fassung
                oder "Aus Datei" nehmen.  */
            {
                static char nurhttp[520];
                scpy(nurhttp,"http://"); scatlim(nurhttp,url+8,500);
                gut=DlHolen(nurhttp,&b,&n,fehler,sizeof(fehler),LadeFortschritt);
            }
#else
            if(PciholeFinden(prog)) gut=PciholeHolen(prog,url,&b,&n,fehler,sizeof(fehler));
            else {
                /*  Ohne PCIHOLE.EXE: dieselbe Adresse mit http:// versuchen.  */
                static char nurhttp[520];
                scpy(nurhttp,"http://"); scatlim(nurhttp,url+8,500);
                gut=DlHolen(nurhttp,&b,&n,fehler,sizeof(fehler),LadeFortschritt);
                if(!gut){ static char f2[800]; scpy(f2,fehler);
                    scpy(fehler,"Fuer https:// fehlt PCIHOLE.EXE (mit CACERT.PEM).  Der Versuch "
                         "mit http:// scheiterte: "); scatlim(fehler,f2,sizeof(fehler)-120); }
            }
#endif
        } else gut=DlHolen(url,&b,&n,fehler,sizeof(fehler),LadeFortschritt);
        if(gut){
            gLadeFenster=NULL; SetCursor(alt); EnableWindow(parent,TRUE);
            PciIdsSchreiben(parent,b,n);
            dl_weg(b);
        } else {
            gLadeFenster=NULL; SetCursor(alt); EnableWindow(parent,TRUE);
            TitelSetzen(gIdsHaupt);
            scpy(t,"Laden fehlgeschlagen:\r\n\r\n"); scatlim(t,fehler,650);
            MessageBoxA(parent,t,"PCI.IDS",MB_OK|MB_ICONEXCLAMATION);
        }
        SetForegroundWindow(parent);
    } else if(wahl==IDNO){
        OPENFILENAMEA ofn; static char datei[MAX_PATH];
        HANDLE h; DWORD n, gelesen=0; BYTE* b;
        datei[0]=0;
        memset(&ofn,0,sizeof(ofn));
        ofn.lStructSize  = OPENFILENAME_SIZE_VERSION_400;
        ofn.hwndOwner    = parent;
        ofn.lpstrFilter  = "PCI.IDS\0pci.ids;*.ids\0Alle Dateien\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrFile    = datei;
        ofn.nMaxFile     = MAX_PATH;
        ofn.lpstrTitle   = "PCI.IDS uebernehmen";
        ofn.Flags        = OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_HIDEREADONLY;
        if(!GetOpenFileNameA(&ofn)) return;
        h=CreateFileA(datei,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if(h==INVALID_HANDLE_VALUE){ MessageBoxA(parent,"Die Datei liess sich nicht oeffnen.","PCI.IDS",MB_OK|MB_ICONSTOP); return; }
        n=GetFileSize(h,NULL);
        if(n==0xFFFFFFFF || n>DL_MAX){ CloseHandle(h); MessageBoxA(parent,"Die Datei ist zu gross.","PCI.IDS",MB_OK|MB_ICONSTOP); return; }
        b=(BYTE*)VirtualAlloc(NULL,n+1,MEM_COMMIT,PAGE_READWRITE);
        if(!b){ CloseHandle(h); return; }
        ReadFile(h,b,n,&gelesen,NULL); CloseHandle(h);
        if(gelesen!=n || !DlPruefe(b,n)){
            VirtualFree(b,0,MEM_RELEASE);
            MessageBoxA(parent,"Das ist keine PCI.IDS (zu klein oder falscher Inhalt).  "
                        "Die vorhandene bleibt.","PCI.IDS",MB_OK|MB_ICONEXCLAMATION);
            return;
        }
        PciIdsSchreiben(parent,b,n);
        VirtualFree(b,0,MEM_RELEASE);
    }
}

#ifndef NUR_ANSEHEN
static void InstallDriver(HWND parent)
{
    OPENFILENAMEA ofn;
    static char datei[MAX_PATH];
    static char kurz[MAX_PATH];
    char sysdir[MAX_PATH], ziel[MAX_PATH], dienst[40], text[800];
    SC_HANDLE scm=NULL, svc=NULL;
    SERVICE_STATUS ss;
    DWORD n, fehler;

    datei[0]=0; kurz[0]=0;

    memset(&ofn,0,sizeof(ofn));
    ofn.lStructSize    = OPENFILENAME_SIZE_VERSION_400;
    ofn.hwndOwner      = parent;
    ofn.lpstrFilter    = "Treiber (*.sys)\0*.sys\0Alle Dateien\0*.*\0";
    ofn.nFilterIndex   = 1;
    ofn.lpstrFile      = datei;
    ofn.nMaxFile       = MAX_PATH;
    ofn.lpstrFileTitle = kurz;
    ofn.nMaxFileTitle  = MAX_PATH;
    ofn.lpstrTitle     = "Treiberdatei waehlen";
    ofn.lpstrDefExt    = "sys";
    ofn.Flags          = OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_HIDEREADONLY;

    if(!GetOpenFileNameA(&ofn)) return;

    if(!DienstnameAus(kurz,dienst)){
        Sagen(parent,
            "Aus diesem Dateinamen laesst sich kein Dienstname bilden.\r\n\r\n"
            "Erlaubt sind Buchstaben, Ziffern, Strich und Unterstrich, "
            "hoechstens 31 Zeichen bis zum Punkt.",
            MB_ICONSTOP);
        return;
    }

    n=GetSystemDirectoryA(sysdir,MAX_PATH-40);
    if(!n) return;
    if(sysdir[n-1]!='\\'){ sysdir[n]='\\'; sysdir[n+1]=0; }
    scpy(ziel,sysdir); scat(ziel,"drivers\\"); scat(ziel,kurz);

    text[0]=0;
    scat(text,"Treiber einrichten?\r\n\r\n");
    scat(text,"Datei     "); scatlim(text,datei,200); scat(text,"\r\n");
    scat(text,"Ziel      "); scatlim(text,ziel,200);  scat(text,"\r\n");
    scat(text,"Dienst    "); scat(text,dienst);       scat(text,"\r\n");
    scat(text,"Start     manuell\r\n\r\n");
    scat(text,"Der Treiber wird kopiert und eingetragen, aber nicht "
              "in den Startablauf gehaengt.");
    if(MessageBoxA(parent,text,"Treiber einrichten",
                   MB_OKCANCEL|MB_ICONQUESTION)!=IDOK) return;

    /* kopieren, falls die Datei nicht schon am Platz liegt */

    if(!ieq(datei,ziel)){
        if(GetFileAttributesA(ziel)!=0xFFFFFFFF){
            if(MessageBoxA(parent,
                "Im Treiberverzeichnis liegt bereits eine Datei dieses Namens.\r\n\r\n"
                "Ueberschreiben?","Treiber einrichten",
                MB_YESNO|MB_ICONEXCLAMATION|MB_DEFBUTTON2)!=IDYES) return;
        }
        if(!CopyFileA(datei,ziel,FALSE)){
            fehler=GetLastError();
            text[0]=0;
            scat(text,"Die Datei liess sich nicht nach system32\\drivers kopieren.\r\n\r\n");
            scat(text,"Fehler "); catnum(text,fehler); scat(text,"\r\n\r\n");
            scat(text,"Ist der Treiber gerade geladen, haelt das System die Datei "
                      "fest; dann zuerst anhalten.");
            Sagen(parent,text,MB_ICONSTOP);
            return;
        }
    }

    /* eintragen */

    scm=OpenSCManagerA(NULL,NULL,SC_MANAGER_ALL_ACCESS);
    if(!scm){
        Sagen(parent,
            "Der Dienstverwalter laesst sich nicht oeffnen -- fehlen "
            "Administratorrechte?",MB_ICONSTOP);
        return;
    }

    svc=CreateServiceA(scm,dienst,dienst,SERVICE_ALL_ACCESS,
                       SERVICE_KERNEL_DRIVER,SERVICE_DEMAND_START,
                       SERVICE_ERROR_NORMAL,ziel,NULL,NULL,NULL,NULL,NULL);

    if(!svc && GetLastError()==ERROR_SERVICE_EXISTS){
        if(MessageBoxA(parent,
            "Ein Dienst dieses Namens ist bereits eingetragen.\r\n\r\n"
            "Den vorhandenen Eintrag ersetzen?","Treiber einrichten",
            MB_YESNO|MB_ICONEXCLAMATION|MB_DEFBUTTON2)==IDYES){

            svc=OpenServiceA(scm,dienst,SERVICE_ALL_ACCESS);
            if(svc){
                ControlService(svc,SERVICE_CONTROL_STOP,&ss);
                DeleteService(svc);
                CloseServiceHandle(svc);
                svc=NULL;
            }
            svc=CreateServiceA(scm,dienst,dienst,SERVICE_ALL_ACCESS,
                               SERVICE_KERNEL_DRIVER,SERVICE_DEMAND_START,
                               SERVICE_ERROR_NORMAL,ziel,NULL,NULL,NULL,NULL,NULL);
        } else {
            CloseServiceHandle(scm);
            return;
        }
    }

    if(!svc){
        fehler=GetLastError();
        text[0]=0;
        scat(text,"Der Dienst liess sich nicht anlegen.\r\n\r\nFehler ");
        catnum(text,fehler);
        if(fehler==ERROR_SERVICE_MARKED_FOR_DELETE)
            scat(text,"\r\n\r\nDer alte Eintrag ist noch in Benutzung und "
                      "verschwindet erst beim naechsten Start.  Danach "
                      "noch einmal versuchen.");
        Sagen(parent,text,MB_ICONSTOP);
        CloseServiceHandle(scm);
        return;
    }

    MerkeGeraet(dienst);

    /* laden? */

    text[0]=0;
    scat(text,"Eingetragen.\r\n\r\n");
    scat(text,"Dienst    "); scat(text,dienst); scat(text,"\r\n");
    scat(text,"Start     manuell\r\n\r\n");
    scat(text,"Jetzt laden?\r\n\r\n");
    scat(text,"Ein fehlerhafter Treiber kann das System dabei anhalten.  "
              "Weil der Start auf manuell steht, kommt die Maschine "
              "danach in jedem Fall wieder hoch.");

    if(MessageBoxA(parent,text,"Treiber einrichten",
                   MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES){
        if(StartServiceA(svc,0,NULL)){
            Sagen(parent,"Der Treiber ist geladen.",MB_ICONINFORMATION);
            gClaimCount=0;
            ReadResourceMap();
            Match();
            BuildTree();
        } else {
            fehler=GetLastError();
            text[0]=0;
            scat(text,"Der Treiber liess sich nicht laden.\r\n\r\nFehler ");
            catnum(text,fehler);
            scat(text,"\r\n\r\nDer Eintrag bleibt bestehen.  Er steht auf "
                      "manuell und wird beim Start nicht angefasst.");
            Sagen(parent,text,MB_ICONSTOP);
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}
#endif /* NUR_ANSEHEN */

/* ---------------- Hauptfenster ---------------- */

#ifdef NUR_ANSEHEN
/*
    "Treiber einrichten" aus der Systemsteuerung: die CPL startet dafuer
    DEVMGR.EXE /einrichten aus system32 -- dort steckt die Dienst-
    verwaltung.  So bleibt die CPL ohne CreateService/StartService und
    ohne eingebetteten Treiber (das Dropper-Muster, das Bkav/Kingsoft/
    Trellix anschlugen); neu ist nur dieser eine Programmstart, nach dem
    Helfer-Muster (av-fehlalarm, Rezept 6).
*/
static void EinrichtenStarten(HWND parent)
{
    char cmd[MAX_PATH+40], exe[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    GetSystemDirectoryA(exe,MAX_PATH-16);
    scat(exe,"\\devmgr.exe");
    if(GetFileAttributesA(exe)==0xFFFFFFFF){
        MessageBoxA(parent,"DEVMGR.EXE fehlt in system32 -- USBINST ein legt es ab.",
                    "Treiber einrichten",MB_OK|MB_ICONSTOP);
        return;
    }
    scpy(cmd,"\""); scat(cmd,exe); scat(cmd,"\" /einrichten");
    memset(&si,0,sizeof(si)); si.cb=sizeof(si);
    if(CreateProcessA(exe,cmd,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi)){
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    } else {
        MessageBoxA(parent,"DEVMGR.EXE liess sich nicht starten.","Treiber einrichten",MB_OK|MB_ICONSTOP);
    }
}
#endif

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch(msg){

    case WM_CREATE:
        gFont=(HFONT)GetStockObject(DEFAULT_GUI_FONT);

        gTree=CreateWindowExA(WS_EX_CLIENTEDGE,WC_TREEVIEWA,"",
                WS_CHILD|WS_VISIBLE|WS_TABSTOP|TVS_HASLINES|TVS_HASBUTTONS|
                TVS_LINESATROOT|TVS_SHOWSELALWAYS,
                8,8,10,10,hwnd,(HMENU)IDC_TREE,gInst,NULL);
        SendMessageA(gTree,WM_SETFONT,(WPARAM)gFont,TRUE);

        MakeImages();
        if(gImages) TreeView_SetImageList(gTree,gImages,TVSIL_NORMAL);

        gBtnProps=CreateWindowExA(0,"BUTTON",S(T_EIGENSCHAFTEN),
                WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_DISABLED|BS_DEFPUSHBUTTON,
                0,0,100,24,hwnd,(HMENU)IDC_PROPS,gInst,NULL);
        gBtnDump=CreateWindowExA(0,"BUTTON",S(T_DIAGNOSE),
                WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                0,0,100,24,hwnd,(HMENU)IDC_DUMP,gInst,NULL);
        gBtnIds=CreateWindowExA(0,"BUTTON",S(T_PCIIDS),
                WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                0,0,100,24,hwnd,(HMENU)IDC_PCIIDS,gInst,NULL);
        /*  Auch in der CPL: dort startet der Knopf DEVMGR.EXE (siehe
            EinrichtenStarten) -- die CPL selbst richtet nichts ein.  */
        gBtnInstall=CreateWindowExA(0,"BUTTON",S(T_TREIBER_EINRICHTEN),
                WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                0,0,100,24,hwnd,(HMENU)IDC_INSTALL,gInst,NULL);
        gBtnRefresh=CreateWindowExA(0,"BUTTON",S(T_AKTUALISIEREN),
                WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                0,0,100,24,hwnd,(HMENU)IDC_REFRESH,gInst,NULL);
        gBtnClose=CreateWindowExA(0,"BUTTON","Schlie&ssen",
                WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                0,0,100,24,hwnd,(HMENU)IDC_CLOSE,gInst,NULL);

        SendMessageA(gBtnProps,WM_SETFONT,(WPARAM)gFont,TRUE);
        SendMessageA(gBtnInstall,WM_SETFONT,(WPARAM)gFont,TRUE);
        SendMessageA(gBtnDump,WM_SETFONT,(WPARAM)gFont,TRUE);
        SendMessageA(gBtnRefresh,WM_SETFONT,(WPARAM)gFont,TRUE);
        SendMessageA(gBtnClose,WM_SETFONT,(WPARAM)gFont,TRUE);
        return 0;

    case WM_SIZE:
        {
            int w=LOWORD(lp), h=HIWORD(lp);
            int bw=96, bh=24, rand=8, luecke=6;    /* 2 x 85 links + 4 x 96 rechts passt in 620 */
            int baumH=h - bh - rand*2 - luecke;
            int by=rand+baumH+luecke;

            MoveWindow(gTree,rand,rand,w-rand*2,baumH,TRUE);
            MoveWindow(gBtnDump,    rand,                 by,85,bh,TRUE);
            MoveWindow(gBtnIds,     rand+85+luecke,       by,85,bh,TRUE);
            MoveWindow(gBtnProps,   w-rand-4*bw-3*luecke, by,bw,bh,TRUE);
            MoveWindow(gBtnInstall, w-rand-3*bw-2*luecke, by,bw,bh,TRUE);
            MoveWindow(gBtnRefresh, w-rand-2*bw-1*luecke, by,bw,bh,TRUE);
            MoveWindow(gBtnClose,   w-rand-1*bw,          by,bw,bh,TRUE);
        }
        return 0;

    case WM_COMMAND:
        switch(LOWORD(wp)){
        case IDC_PROPS:   ShowProperties(hwnd); return 0;
#ifndef NUR_ANSEHEN
        case IDC_INSTALL: InstallDriver(hwnd);  return 0;
#else
        case IDC_INSTALL: EinrichtenStarten(hwnd); return 0;
#endif
        case IDC_DUMP:    DumpDiagnose(hwnd);   return 0;
        case IDC_PCIIDS:
            /*  Das Laden sperrt das Fenster; danach hatte kein Steuer-
                element mehr den Fokus, und Tastenkuerzel liefen ins
                Leere.  */
            PciIdsFenster(hwnd);
            SetFocus(gTree);
            return 0;
        case IDC_REFRESH:
            gPciFehlt = !GetList();          /* inzwischen gestartet? */
            gClaimCount=0;
            ReadResourceMap();
            Match();
            BuildTree();
            return 0;
        case IDC_CLOSE:   DestroyWindow(hwnd); return 0;
        }
        return 0;

    case WM_NOTIFY:
        {
            NM_TREEVIEW* nt=(NM_TREEVIEW*)lp;
            if(nt->hdr.idFrom==IDC_TREE){
                if(nt->hdr.code==TVN_SELCHANGED){
                    gSel=(int)nt->itemNew.lParam;
                    EnableWindow(gBtnProps, gSel>=0);
                } else if(nt->hdr.code==NM_DBLCLK){
                    if(gSel>=0) ShowProperties(hwnd);
                }
            }
        }
        return 0;

    case WM_DESTROY:
        if(gImages) ImageList_Destroy(gImages);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd,msg,wp,lp);
}

/* ------------------------------------------------------------------ */
/*  Als Systemsteuerungselement                                         */
/* ------------------------------------------------------------------ */

/*
    Eine .cpl-Datei ist eine DLL, die eine einzige Funktion exportiert.
    Das Systemsteuerungsprogramm sucht beim Start alle .cpl-Dateien in
    system32, laedt sie und fragt ueber CPlApplet Name, Beschreibung und
    Symbol ab.

    sysdm.cpl muss also nicht angefasst werden.  Zum Entfernen genuegt
    Loeschen.
*/

#define CPL_INIT        1
#define CPL_GETCOUNT    2
#define CPL_INQUIRE     3
#define CPL_DBLCLK      5
#define CPL_STOP        6
#define CPL_EXIT        7
#define CPL_NEWINQUIRE  8

typedef struct {
    DWORD dwSize, dwFlags, dwHelpContext;
    LONG  lData;
    HICON hIcon;
    CHAR  szName[32];
    CHAR  szInfo[64];
    CHAR  szHelpFile[128];
} NEWCPLINFOA;

typedef struct { int idIcon, idName, idInfo; LONG lData; } CPLINFO;

static BOOL gRegistered = FALSE;

static BOOL Vorbereiten(HWND parent)
{
    INITCOMMONCONTROLSEX ic;

    ic.dwSize = sizeof(ic);
    ic.dwICC  = ICC_TREEVIEW_CLASSES;
    InitCommonControlsEx(&ic);

    PciIdsInit();

    /*  Ohne PCISCAN (Dienst nicht eingerichtet oder nicht gestartet) geht
        es trotzdem weiter: die PCI-Liste bleibt leer, der Baum sagt es,
        USB und alles andere wird angezeigt.  Frueher brach der
        Geraetemanager hier ab -- entgegen der Absicht von GetList.  */
    gPciFehlt = !GetList();

    gClaimCount = 0;
    ReadResourceMap();
    Match();

    if(!gRegistered){
        WNDCLASSA wc;
        memset(&wc,0,sizeof(wc));
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = gInst;
        wc.hIcon         = LoadIconA(gInst,MAKEINTRESOURCEA(ICO_APP));
        wc.hCursor       = LoadCursorA(NULL,(LPCSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
        wc.lpszClassName = "Nt4DevMgr";
        if(RegisterClassA(&wc)) gRegistered=TRUE;
    }

    return TRUE;
}

static void FensterLaufenLassen(void)
{
    HWND hwnd;
    MSG  msg;

    /*  Hoechstens so gross wie der freie Bildschirmbereich: mit 580 x 520
        lagen die Knoepfe bei 640 x 480 unter dem Bildschirmrand.  */
    {
        RECT wa; int W=620, H=520, ww, wh;
        if(!SystemParametersInfoA(SPI_GETWORKAREA,0,&wa,0)){
            wa.left=0; wa.top=0; wa.right=GetSystemMetrics(SM_CXSCREEN); wa.bottom=GetSystemMetrics(SM_CYSCREEN);
        }
        ww=wa.right-wa.left; wh=wa.bottom-wa.top;
        if(W>ww) W=ww;
        if(H>wh) H=wh;
        hwnd=CreateWindowExA(0,"Nt4DevMgr","Geraetemanager",
                WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                wa.left+(ww-W)/2, wa.top+(wh-H)/2, W, H,
                NULL,NULL,gInst,NULL);
    }
    if(!hwnd) return;

    BuildTree();
    SetFocus(gTree);

#ifndef NUR_ANSEHEN
    /*  Aus der CPL gestartet (Knopf "Treiber einrichten"): gleich den
        Dialog oeffnen.  */
    {
        const char* c=GetCommandLineA();
        int k;
        for(k=0;c[k];k++){
            if(c[k]=='/' && (c[k+1]=='e'||c[k+1]=='E') && (c[k+2]=='i'||c[k+2]=='I')){
                PostMessageA(hwnd,WM_COMMAND,IDC_INSTALL,0);
                break;
            }
        }
    }
#endif

    /*
        Damit man ohne Nachsehen erkennt, woher die Namen kommen.
    */
    TitelSetzen(hwnd);

    while(GetMessageA(&msg,NULL,0,0)){
        if(!IsDialogMessageA(hwnd,&msg)){
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
}

BOOL __stdcall DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);
    if(reason==DLL_PROCESS_ATTACH) gInst=inst;
    return TRUE;
}

LONG __stdcall CPlApplet(HWND hwndCPl, UINT msg, LPARAM lp1, LPARAM lp2)
{
    UNREFERENCED_PARAMETER(lp1);

    switch(msg){

    case CPL_INIT:
        return 1;

    case CPL_GETCOUNT:
        return 1;

    case CPL_INQUIRE:
        {
            /*
                Die alte Form verlangt Ressourcen-Ids; wir haben keine
                Ressourcen.  Ein gueltiger Rumpf genuegt -- die
                Systemsteuerung fragt anschliessend mit CPL_NEWINQUIRE
                nach, und dort stehen die Texte im Klartext.
            */
            CPLINFO* ci=(CPLINFO*)lp2;
            ci->idIcon=ICO_APP; ci->idName=0; ci->idInfo=0; ci->lData=0;
            return 0;
        }

    case CPL_NEWINQUIRE:
        {
            NEWCPLINFOA* ni=(NEWCPLINFOA*)lp2;
            memset(ni,0,sizeof(NEWCPLINFOA));
            ni->dwSize=sizeof(NEWCPLINFOA);
            ni->hIcon=LoadIconA(gInst,MAKEINTRESOURCEA(ICO_APP));
            scpy(ni->szName,"Geraetemanager");
            scpy(ni->szInfo,"Zeigt die PCI-Geraete und ihre Treiber.");
            return 0;
        }

    case CPL_DBLCLK:
        if(Vorbereiten(hwndCPl)) FensterLaufenLassen();
        return 0;

    case CPL_STOP:
    case CPL_EXIT:
        return 0;
    }

    return 0;
}

int __stdcall WinMainCRTStartup(void)
{
    gInst=GetModuleHandleA(NULL);

    if(!Vorbereiten(NULL)) ExitProcess(1);
    FensterLaufenLassen();

    ExitProcess(0);
    return 0;
}
