/*
 *  pcidl.h -- PCI.IDS per HTTP laden, fuer den Geraetemanager.
 *
 *  Nur einfaches HTTP: NT 4.0 kann kein TLS 1.2, das fast jeder Server
 *  heute fuer HTTPS verlangt.  (HTTPS: PCIHOLE.EXE mit wolfSSL, siehe devmgr.c.)  Leitet der Server auf https:// um, sagt
 *  DlHolen das deutlich (statt still zu scheitern).
 *
 *  WinSock 1.1 aus wsock32.dll, erst bei Bedarf geladen -- ohne TCP/IP
 *  startet der Geraetemanager trotzdem.  HTTP/1.1 mit Connection: close
 *  (1.0 lehnen manche Server ab: 426); Rumpf auch "chunked".
 *
 *  Mit -DDL_TEST_LINUX uebersetzt dieselbe Datei unter Linux (Pruefstand:
 *  gegen einen lokalen Testserver), damit die HTTP-Logik ohne NT-Netz
 *  geprueft werden kann.
 */

#define DL_MAX        (12*1024*1024)
#define DL_UMLEITUNGEN 4

#ifdef DL_TEST_LINUX
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
typedef unsigned char BYTE; typedef unsigned int DWORD; typedef int BOOL;
#define TRUE 1
#define FALSE 0
typedef int DL_SOCK;
#define DL_UNGUELTIG (-1)
static void* dl_neu(DWORD n){ return malloc(n); }
static void  dl_weg(void* p){ free(p); }
static BOOL  dl_start(char* f,int m){ (void)f;(void)m; return TRUE; }
static void  dl_ende(void){}
static DL_SOCK dl_verbinde(const char* host, int port, char* f, int m);
static int   dl_senden(DL_SOCK s,const char* b,int n){ return (int)send(s,b,n,0); }
static int   dl_lesen(DL_SOCK s,char* b,int n){ return (int)recv(s,b,n,0); }
static void  dl_zu(DL_SOCK s){ close(s); }
#else
#include <winsock.h>
typedef SOCKET DL_SOCK;
#define DL_UNGUELTIG INVALID_SOCKET
typedef int      (PASCAL *dl_WSAStartup_t)(WORD,LPWSADATA);
typedef int      (PASCAL *dl_WSACleanup_t)(void);
typedef SOCKET   (PASCAL *dl_socket_t)(int,int,int);
typedef int      (PASCAL *dl_connect_t)(SOCKET,const struct sockaddr*,int);
typedef int      (PASCAL *dl_send_t)(SOCKET,const char*,int,int);
typedef int      (PASCAL *dl_recv_t)(SOCKET,char*,int,int);
typedef int      (PASCAL *dl_closesocket_t)(SOCKET);
typedef struct hostent* (PASCAL *dl_gethostbyname_t)(const char*);
typedef unsigned long (PASCAL *dl_inet_addr_t)(const char*);
typedef u_short  (PASCAL *dl_htons_t)(u_short);
typedef int      (PASCAL *dl_setsockopt_t)(SOCKET,int,int,const char*,int);
static HMODULE dl_ws;
static dl_WSAStartup_t dl_pWSAStartup; static dl_WSACleanup_t dl_pWSACleanup;
static dl_socket_t dl_psocket; static dl_connect_t dl_pconnect; static dl_send_t dl_psend;
static dl_recv_t dl_precv; static dl_closesocket_t dl_pclosesocket;
static dl_gethostbyname_t dl_pgethostbyname; static dl_inet_addr_t dl_pinet_addr;
static dl_htons_t dl_phtons; static dl_setsockopt_t dl_psetsockopt;
static void* dl_neu(DWORD n){ return VirtualAlloc(NULL,n,MEM_COMMIT,PAGE_READWRITE); }
static void  dl_weg(void* p){ if(p) VirtualFree(p,0,MEM_RELEASE); }
static void  dl_text(char* f,int m,const char* t){ int i=0; while(t[i] && i<m-1){ f[i]=t[i]; i++; } f[i]=0; }
static BOOL dl_start(char* f,int m)
{
    WSADATA wd;
    if(!dl_ws) dl_ws=LoadLibraryA("wsock32.dll");
    if(!dl_ws){ dl_text(f,m,"WinSock (wsock32.dll) fehlt -- ist TCP/IP eingerichtet?"); return FALSE; }
#define DL_HOL(n) dl_p##n=(dl_##n##_t)GetProcAddress(dl_ws,#n); if(!dl_p##n){ dl_text(f,m,"wsock32.dll: " #n " fehlt"); return FALSE; }
    DL_HOL(WSAStartup) DL_HOL(WSACleanup) DL_HOL(socket) DL_HOL(connect) DL_HOL(send)
    DL_HOL(recv) DL_HOL(closesocket) DL_HOL(gethostbyname) DL_HOL(inet_addr) DL_HOL(htons)
    DL_HOL(setsockopt)
#undef DL_HOL
    if(dl_pWSAStartup(0x0101,&wd)!=0){ dl_text(f,m,"WinSock laesst sich nicht starten -- ist TCP/IP eingerichtet?"); return FALSE; }
    return TRUE;
}
static void dl_ende(void){ if(dl_pWSACleanup) dl_pWSACleanup(); }
static DL_SOCK dl_verbinde(const char* host, int port, char* f, int m);
static int  dl_senden(DL_SOCK s,const char* b,int n){ return dl_psend(s,b,n,0); }
static int  dl_lesen(DL_SOCK s,char* b,int n){ return dl_precv(s,b,n,0); }
static void dl_zu(DL_SOCK s){ dl_pclosesocket(s); }
#endif

static int  dl_len(const char* s){ int n=0; while(s[n]) n++; return n; }
static void dl_cpy(char* d,const char* s,int m){ int i=0; while(s[i] && i<m-1){ d[i]=s[i]; i++; } d[i]=0; }
static void dl_cat(char* d,const char* s,int m){ int i=dl_len(d),j=0; while(s[j] && i<m-1) d[i++]=s[j++]; d[i]=0; }
static char dl_klein(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static BOOL dl_beginnt(const char* s,const char* mit)
{ int i; for(i=0;mit[i];i++) if(dl_klein(s[i])!=mit[i]) return FALSE; return TRUE; }
static void dl_zahl(char* d,int v,int m){ char t[12]; int n=0,i; if(v==0) t[n++]='0'; while(v>0){ t[n++]=(char)('0'+v%10); v/=10; } for(i=0;i<n && i<m-1;i++) d[i]=t[n-1-i]; d[i]=0; }

#ifdef DL_TEST_LINUX
static DL_SOCK dl_verbinde(const char* host, int port, char* f, int m)
{
    struct hostent* he=gethostbyname(host); struct sockaddr_in a; DL_SOCK s; struct timeval tv={20,0};
    if(!he){ dl_cpy(f,"Rechnername unbekannt: ",m); dl_cat(f,host,m); return DL_UNGUELTIG; }
    s=socket(AF_INET,SOCK_STREAM,0); if(s<0){ dl_cpy(f,"socket fehlgeschlagen",m); return DL_UNGUELTIG; }
    setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_port=htons((unsigned short)port);
    memcpy(&a.sin_addr,he->h_addr_list[0],4);
    if(connect(s,(struct sockaddr*)&a,sizeof(a))!=0){ close(s); dl_cpy(f,"Verbindung abgelehnt: ",m); dl_cat(f,host,m); return DL_UNGUELTIG; }
    return s;
}
#else
static DL_SOCK dl_verbinde(const char* host, int port, char* f, int m)
{
    struct sockaddr_in a; DL_SOCK s; unsigned long ip; int zeit=20000;
    ip=dl_pinet_addr(host);
    if(ip==INADDR_NONE){
        struct hostent* he=dl_pgethostbyname(host);
        if(!he || !he->h_addr_list || !he->h_addr_list[0]){
            dl_cpy(f,"Rechnername unbekannt: ",m); dl_cat(f,host,m);
            dl_cat(f,"  (Netz, DNS oder Proxy?)",m); return DL_UNGUELTIG; }
        memcpy(&ip,he->h_addr_list[0],4);
    }
    s=dl_psocket(AF_INET,SOCK_STREAM,0);
    if(s==INVALID_SOCKET){ dl_cpy(f,"socket fehlgeschlagen",m); return DL_UNGUELTIG; }
    dl_psetsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&zeit,sizeof(zeit));
    memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_port=dl_phtons((u_short)port);
    a.sin_addr.s_addr=ip;
    if(dl_pconnect(s,(struct sockaddr*)&a,sizeof(a))!=0){
        dl_pclosesocket(s); dl_cpy(f,"Keine Verbindung zu ",m); dl_cat(f,host,m); return DL_UNGUELTIG; }
    return s;
}
#endif


/*  HTTP/1.1 "Transfer-Encoding: chunked": Rumpf an Ort und Stelle
    zusammensetzen.  Rueckgabe: neue Laenge, oder 0xFFFFFFFF bei Unsinn.  */
static DWORD dl_entstueckle(char* b, DWORD n)
{
    DWORD lies = 0, schreib = 0;
    for (;;) {
        DWORD laenge = 0; int ziffern = 0;
        while (lies < n) {
            char c = b[lies];
            int w = (c>='0'&&c<='9') ? c-'0' : (c>='a'&&c<='f') ? c-'a'+10 : (c>='A'&&c<='F') ? c-'A'+10 : -1;
            if (w < 0) break;
            laenge = laenge*16 + (DWORD)w; lies++; ziffern++;
            if (ziffern > 7) return 0xFFFFFFFF;
        }
        if (!ziffern) return 0xFFFFFFFF;
        while (lies < n && b[lies] != '\n') lies++;          /* Erweiterungen, CRLF */
        if (lies >= n) return 0xFFFFFFFF;
        lies++;
        if (laenge == 0) return schreib;                      /* letztes Stueck */
        if (lies + laenge > n) return 0xFFFFFFFF;
        { DWORD i; for (i = 0; i < laenge; i++) b[schreib + i] = b[lies + i]; }
        schreib += laenge; lies += laenge;
        if (lies + 1 < n && b[lies] == '\r') lies++;
        if (lies < n && b[lies] == '\n') lies++;
    }
}

/*  Steht "Transfer-Encoding: chunked" im Kopf?  */
static int dl_ist_chunked(const char* b, DWORD kopf)
{
    DWORD i;
    for (i = 0; i + 26 < kopf; i++) {
        if ((i == 0 || b[i-1] == '\n') && dl_beginnt(b + i, "transfer-encoding:")) {
            DWORD k = i + 18;
            while (k < kopf && b[k] != '\n') {
                if (dl_beginnt(b + k, "chunked")) return 1;
                k++;
            }
        }
    }
    return 0;
}

/*  http://host[:port]/pfad zerlegen.  0 = in Ordnung, 1 = https, -1 = Unsinn.  */
static int dl_zerlege(const char* url, char* host, int hm, int* port, char* pfad, int pm)
{
    const char* p=url; int i=0;
    while(*p==' ') p++;
    if(dl_beginnt(p,"https://")) return 1;
    if(!dl_beginnt(p,"http://")) return -1;
    p+=7;
    while(*p && *p!='/' && *p!=':' && i<hm-1) host[i++]=*p++;
    host[i]=0; if(!i) return -1;
    *port=80;
    if(*p==':'){ int v=0; p++; while(*p>='0'&&*p<='9') v=v*10+(*p++-'0'); if(v<=0||v>65535) return -1; *port=v; }
    if(*p=='/') dl_cpy(pfad,p,pm); else dl_cpy(pfad,"/",pm);
    return 0;
}

/*  Sieht der Inhalt wie eine pci.ids aus?  */
static BOOL DlPruefe(const BYTE* b, DWORD n)
{
    DWORD i; BOOL intel=FALSE, klasse=FALSE;
    if(n<100000 || b[0]!='#') return FALSE;
    for(i=0;i+20<n && !(intel&&klasse);i++){
        if(b[i]!='\n') continue;
        if(b[i+1]=='8'&&b[i+2]=='0'&&b[i+3]=='8'&&b[i+4]=='6'&&b[i+5]==' '&&b[i+6]==' '&&b[i+7]=='I') intel=TRUE;
        if(b[i+1]=='C'&&b[i+2]==' '&&b[i+3]=='0'&&b[i+4]=='1'&&b[i+5]==' ') klasse=TRUE;
    }
    return intel && klasse;
}

/*
 *  url laden.  Bei Erfolg: *daten (mit dl_weg freigeben), *laenge = Rumpf.
 *  fortschritt(n) wird unterwegs mit der Anzahl gelesener Bytes gerufen
 *  (darf NULL sein).  Fehlertext in fehler.
 */
static BOOL DlHolen(const char* url, BYTE** daten, DWORD* laenge, char* fehler, int fm,
                    void (*fortschritt)(DWORD))
{
    /*  statisch: unter 4 KB Stapel bleiben (ohne CRT keine Stapelpruefung),
        und es laedt ohnehin immer nur einer.  */
    static char host[128], pfad[512], jetzt[640], anfrage[900], ort[640];
    int  port, runde, art;
    BYTE* b=NULL;

    *daten=NULL; *laenge=0; fehler[0]=0;
    dl_cpy(jetzt,url,sizeof(jetzt));
    if(!dl_start(fehler,fm)) return FALSE;

    for(runde=0; runde<=DL_UMLEITUNGEN; runde++){
        DL_SOCK s; DWORD n=0, kopfende=0, i; int r, status=0;

        art=dl_zerlege(jetzt,host,sizeof(host),&port,pfad,sizeof(pfad));
        if(art==1){
            dl_cpy(fehler,"Der Server verlangt HTTPS (",fm); dl_cat(fehler,jetzt,fm);
            dl_cat(fehler,").  NT 4.0 beherrscht das heute uebliche TLS 1.2 nicht.  "
                   "Bitte eine http://-Adresse eintragen (Registry: HKLM\\SOFTWARE\\"
                   "Geraetemanager, PciIdsUrl) oder die Datei auf einem anderen Rechner "
                   "laden und hier 'Aus Datei' waehlen.",fm);
            dl_ende(); return FALSE;
        }
        if(art<0){ dl_cpy(fehler,"Adresse nicht verstanden: ",fm); dl_cat(fehler,jetzt,fm); dl_ende(); return FALSE; }

        s=dl_verbinde(host,port,fehler,fm);
        if(s==DL_UNGUELTIG){ dl_ende(); return FALSE; }

        anfrage[0]=0;
        dl_cat(anfrage,"GET ",sizeof(anfrage)); dl_cat(anfrage,pfad,sizeof(anfrage));
        dl_cat(anfrage," HTTP/1.1\r\nHost: ",sizeof(anfrage)); dl_cat(anfrage,host,sizeof(anfrage));
        if(port!=80){ char t[8]; dl_zahl(t,port,sizeof(t)); dl_cat(anfrage,":",sizeof(anfrage)); dl_cat(anfrage,t,sizeof(anfrage)); }
        dl_cat(anfrage,"\r\nUser-Agent: NT4-Geraetemanager/1.9\r\nAccept: */*\r\n"
                       "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",sizeof(anfrage));
        if(dl_senden(s,anfrage,dl_len(anfrage))<=0){ dl_zu(s); dl_cpy(fehler,"Senden fehlgeschlagen",fm); dl_ende(); return FALSE; }

        if(!b) b=(BYTE*)dl_neu(DL_MAX+1);
        if(!b){ dl_zu(s); dl_cpy(fehler,"Kein Speicher",fm); dl_ende(); return FALSE; }
        while(n<DL_MAX && (r=dl_lesen(s,(char*)b+n,(int)(DL_MAX-n>65536?65536:DL_MAX-n)))>0){
            n+=(DWORD)r;
            if(fortschritt) fortschritt(n);
        }
        dl_zu(s);
        b[n]=0;
        if(n>=DL_MAX){ dl_cpy(fehler,"Antwort zu gross",fm); dl_weg(b); dl_ende(); return FALSE; }

        for(i=0;i+3<n;i++) if(b[i]=='\r'&&b[i+1]=='\n'&&b[i+2]=='\r'&&b[i+3]=='\n'){ kopfende=i+4; break; }
        if(!kopfende || !dl_beginnt((char*)b,"http/1.")){
            dl_cpy(fehler,"Keine gueltige HTTP-Antwort (Verbindung abgebrochen?)",fm); dl_weg(b); dl_ende(); return FALSE; }
        for(i=0;i<kopfende && b[i]!=' ';i++){}
        while(i<kopfende && b[i]==' ') i++;
        while(i<kopfende && b[i]>='0'&&b[i]<='9') status=status*10+(b[i++]-'0');

        if(status==301||status==302||status==303||status==307||status==308){
            ort[0]=0;
            for(i=0;i<kopfende;i++){
                if((i==0||b[i-1]=='\n') && dl_beginnt((char*)b+i,"location:")){
                    int k=0; i+=9; while(b[i]==' ') i++;
                    while(i<kopfende && b[i]!='\r' && k<(int)sizeof(ort)-1) ort[k++]=(char)b[i++];
                    ort[k]=0; break;
                }
            }
            if(!ort[0]){ dl_cpy(fehler,"Umleitung ohne Ziel",fm); dl_weg(b); dl_ende(); return FALSE; }
            if(ort[0]=='/'){          /* relativ: gleicher Rechner */
                char t[8];
                dl_cpy(jetzt,"http://",sizeof(jetzt)); dl_cat(jetzt,host,sizeof(jetzt));
                if(port!=80){ dl_zahl(t,port,sizeof(t)); dl_cat(jetzt,":",sizeof(jetzt)); dl_cat(jetzt,t,sizeof(jetzt)); }
                dl_cat(jetzt,ort,sizeof(jetzt));
            } else dl_cpy(jetzt,ort,sizeof(jetzt));
            continue;
        }
        if(status!=200){
            char t[8]; dl_zahl(t,status,sizeof(t));
            dl_cpy(fehler,"Der Server antwortet mit ",fm); dl_cat(fehler,t,fm);
            dl_cat(fehler," (",fm); dl_cat(fehler,jetzt,fm); dl_cat(fehler,")",fm);
            dl_weg(b); dl_ende(); return FALSE;
        }
        /*  Rumpf nach vorn schieben  */
        {
            int stuecke=dl_ist_chunked((char*)b,kopfende);
            for(i=0;i+kopfende<n;i++) b[i]=b[i+kopfende];
            n-=kopfende; b[n]=0;
            if(stuecke){      /* HTTP/1.1: Rumpf darf in Stuecken kommen */
                DWORD neu=dl_entstueckle((char*)b,n);
                if(neu==0xFFFFFFFF){ dl_cpy(fehler,"Antwort in Stuecken (chunked) ist unvollstaendig",fm); dl_weg(b); dl_ende(); return FALSE; }
                n=neu; b[n]=0;
            }
        }
        if(!DlPruefe(b,n)){
            dl_cpy(fehler,"Die geladene Datei ist keine PCI.IDS (zu klein oder falscher Inhalt).  "
                   "Die vorhandene bleibt.",fm);
            dl_weg(b); dl_ende(); return FALSE;
        }
        *daten=b; *laenge=n; dl_ende(); return TRUE;
    }
    dl_cpy(fehler,"Zu viele Umleitungen",fm); dl_weg(b); dl_ende(); return FALSE;
}
