/*
 *  pcihole.c -- PCI.IDS ueber HTTPS holen, fuer den Geraetemanager.
 *
 *      PCIHOLE <https://...> <zieldatei>
 *
 *  TLS 1.2 mit wolfSSL 5.9.1 (wolfssl_nt4.a).  Geprueft werden die
 *  Zertifikatskette gegen CACERT.PEM (neben dem Programm) UND der
 *  Rechnername.  Fehlt CACERT.PEM, wird NICHT ohne Pruefung geladen --
 *  das waere schlechter als kein HTTPS, weil es Sicherheit nur vorgaukelt.
 *
 *  Ausgabe fuer den Geraetemanager (stdout, zeilenweise):
 *      KB <n>          Fortschritt
 *      OK <bytes>      fertig, Zieldatei geschrieben
 *      FEHLER <text>   deutscher Klartext
 *  Rueckgabe 0 = gut.
 *
 *  Umleitungen nur auf https:// (eine auf http:// waere eine stille
 *  Herabstufung).  Ob der Inhalt eine PCI.IDS ist, prueft der Aufrufer.
 */
#include "user_settings.h"
#include <winsock2.h>
#include <windows.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXGROESSE (12*1024*1024)

/*  msvcrt.dll von NT 4.0 kennt strcpy_s/strtok_s nicht (erst VC 2005);
    wolfssl_nt4.a importiert sie aber.  Eigene Fassungen, ueber die
    Importzeiger bereitgestellt -- sonst startet das Programm unter NT 4.0
    gar nicht ("Einsprungpunkt nicht gefunden").  */
static int __cdecl eigen_strcpy_s(char* d, size_t n, const char* s)
{
    size_t i;
    if (!d || !s || n == 0) return 22;
    for (i = 0; i < n; i++) { d[i] = s[i]; if (!s[i]) return 0; }
    d[0] = 0; return 34;
}
static char* __cdecl eigen_strtok_s(char* s, const char* trenn, char** kontext)
{
    char* anfang;
    if (!s) s = *kontext;
    if (!s) return NULL;
    while (*s && strchr(trenn, *s)) s++;
    if (!*s) { *kontext = s; return NULL; }
    anfang = s;
    while (*s && !strchr(trenn, *s)) s++;
    if (*s) *s++ = 0;
    *kontext = s;
    return anfang;
}
void* _imp__strcpy_s = (void*)eigen_strcpy_s;
void* _imp__strtok_s = (void*)eigen_strtok_s;


/*  HTTP/1.1 "Transfer-Encoding: chunked": Rumpf an Ort und Stelle
    zusammensetzen.  Rueckgabe: neue Laenge, oder 0xFFFFFFFF bei Unsinn.  */
static DWORD entstueckle(char* b, DWORD n)
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
static int ist_chunked(const char* b, DWORD kopf)
{
    DWORD i;
    for (i = 0; i + 26 < kopf; i++) {
        if ((i == 0 || b[i-1] == '\n') && _strnicmp(b + i, "transfer-encoding:", 18) == 0) {
            DWORD k = i + 18;
            while (k < kopf && b[k] != '\n') {
                if (_strnicmp(b + k, "chunked", 7) == 0) return 1;
                k++;
            }
        }
    }
    return 0;
}

static void fehler(const char* text)
{
    printf("FEHLER %s\n", text);
    fflush(stdout);
}

static const char* klartext(int e, char* puffer, int n)
{
    switch (e) {
    case ASN_NO_SIGNER_E:
        return "Das Serverzertifikat stammt von keiner bekannten Zertifizierungsstelle "
               "(CACERT.PEM zu alt oder unvollstaendig?).";
    case DOMAIN_NAME_MISMATCH:
        return "Das Serverzertifikat gilt nicht fuer diesen Rechnernamen.";
    case ASN_AFTER_DATE_E:
        return "Das Serverzertifikat ist abgelaufen -- oder die Uhr dieses Rechners geht falsch.";
    case ASN_BEFORE_DATE_E:
        return "Das Serverzertifikat ist noch nicht gueltig -- steht die Uhr dieses Rechners richtig?";
    case ASN_SIG_CONFIRM_E:
        return "Die Unterschrift des Serverzertifikats stimmt nicht.";
    case ASN_SELF_SIGNED_E:
        return "Der Server benutzt ein selbst ausgestelltes Zertifikat.";
    case SOCKET_ERROR_E: case SOCKET_PEER_CLOSED_E:
        return "Die Verbindung wurde unterbrochen.";
    case MATCH_SUITE_ERROR: case VERSION_ERROR: case FATAL_ERROR:
        return "Der Server und dieses Programm haben kein gemeinsames Verfahren gefunden "
               "(TLS 1.2 mit AES-GCM, ChaCha20, ECC, RSA).";
    }
    wolfSSL_ERR_error_string_n((unsigned long)e, puffer, (unsigned long)n);
    return puffer;
}

/*  https://host[:port]/pfad  */
static int zerlege(const char* url, char* host, int hm, int* port, char* pfad, int pm)
{
    const char* p = url; int i = 0;
    if (_strnicmp(p, "https://", 8) != 0) return -1;
    p += 8;
    while (*p && *p != '/' && *p != ':' && i < hm - 1) host[i++] = *p++;
    host[i] = 0;
    if (!i) return -1;
    *port = 443;
    if (*p == ':') { *port = atoi(p + 1); while (*p && *p != '/') p++; if (*port <= 0 || *port > 65535) return -1; }
    if (*p == '/') { strncpy(pfad, p, pm - 1); pfad[pm - 1] = 0; } else strcpy(pfad, "/");
    return 0;
}

static SOCKET verbinde(const char* host, int port)
{
    struct hostent* he; struct sockaddr_in a; SOCKET s; unsigned long ip; int zeit = 30000;
    char t[300];
    ip = inet_addr(host);
    if (ip == INADDR_NONE) {
        he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) {
            _snprintf(t, sizeof(t), "Rechnername unbekannt: %s  (Netz, DNS oder Proxy?)", host);
            fehler(t); return INVALID_SOCKET;
        }
        memcpy(&ip, he->h_addr_list[0], 4);
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { fehler("socket fehlgeschlagen"); return s; }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&zeit, sizeof(zeit));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = ip;
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        closesocket(s);
        _snprintf(t, sizeof(t), "Keine Verbindung zu %s:%d", host, port);
        fehler(t); return INVALID_SOCKET;
    }
    return s;
}

/*  Eine Anfrage.  0 = fertig (Rumpf in *b ab *rumpf), 1 = Umleitung
    (neue Adresse in url), -1 = Fehler (gemeldet).  */
static int abruf(WOLFSSL_CTX* ctx, char* url, int um, char* b, DWORD* n, DWORD* rumpf)
{
    char host[256], pfad[1024], anfrage[1600], t[600], e[160];
    int port, r, status = 0; DWORD i, kopf = 0, kb_alt = 0;
    SOCKET s; WOLFSSL* ssl;

    if (zerlege(url, host, sizeof(host), &port, pfad, sizeof(pfad)) != 0) {
        _snprintf(t, sizeof(t), "Adresse nicht verstanden (nur https://): %s", url);
        fehler(t); return -1;
    }
    s = verbinde(host, port);
    if (s == INVALID_SOCKET) return -1;

    ssl = wolfSSL_new(ctx);
    if (!ssl) { closesocket(s); fehler("wolfSSL_new fehlgeschlagen"); return -1; }
    wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, (word16)strlen(host));
    wolfSSL_check_domain_name(ssl, host);            /* Rechnername pruefen! */
    wolfSSL_set_fd(ssl, (int)s);
    if (wolfSSL_connect(ssl) != WOLFSSL_SUCCESS) {
        int err = wolfSSL_get_error(ssl, 0);
        _snprintf(t, sizeof(t), "HTTPS-Verbindung zu %s fehlgeschlagen: %s", host, klartext(err, e, sizeof(e)));
        fehler(t);
        wolfSSL_free(ssl); closesocket(s); return -1;
    }

    _snprintf(anfrage, sizeof(anfrage),
              "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: NT4-Geraetemanager/1.9 (PCIHOLE)\r\n"
              "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", pfad, host);
    if (wolfSSL_write(ssl, anfrage, (int)strlen(anfrage)) <= 0) {
        fehler("Senden fehlgeschlagen"); wolfSSL_free(ssl); closesocket(s); return -1;
    }
    *n = 0;
    for (;;) {
        r = wolfSSL_read(ssl, b + *n, (int)(MAXGROESSE - *n > 16384 ? 16384 : MAXGROESSE - *n));
        if (r <= 0) break;
        *n += (DWORD)r;
        if (*n / 65536 != kb_alt) { kb_alt = *n / 65536; printf("KB %lu\n", (unsigned long)(*n / 1024)); fflush(stdout); }
        if (*n >= MAXGROESSE) break;
    }
    wolfSSL_shutdown(ssl); wolfSSL_free(ssl); closesocket(s);
    b[*n] = 0;
    if (*n >= MAXGROESSE) { fehler("Antwort zu gross"); return -1; }

    for (i = 0; i + 3 < *n; i++)
        if (b[i]=='\r' && b[i+1]=='\n' && b[i+2]=='\r' && b[i+3]=='\n') { kopf = i + 4; break; }
    if (!kopf || _strnicmp(b, "HTTP/1.", 7) != 0) { fehler("Keine gueltige HTTP-Antwort (Verbindung abgebrochen?)"); return -1; }
    status = atoi(strchr(b, ' ') ? strchr(b, ' ') + 1 : "0");

    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        char* ort = NULL;
        for (i = 0; i < kopf; i++)
            if ((i == 0 || b[i-1] == '\n') && _strnicmp(b + i, "location:", 9) == 0) { ort = b + i + 9; break; }
        if (!ort) { fehler("Umleitung ohne Ziel"); return -1; }
        while (*ort == ' ') ort++;
        for (i = 0; ort[i] && ort[i] != '\r' && ort[i] != '\n'; i++) {}
        ort[i] = 0;
        if (ort[0] == '/') {
            if (port == 443) _snprintf(url, um, "https://%s%s", host, ort);
            else _snprintf(url, um, "https://%s:%d%s", host, port, ort);
        } else if (_strnicmp(ort, "https://", 8) == 0) {
            strncpy(url, ort, um - 1); url[um - 1] = 0;
        } else {
            _snprintf(t, sizeof(t), "Umleitung auf eine unverschluesselte Adresse abgelehnt: %s", ort);
            fehler(t); return -1;
        }
        return 1;
    }
    if (status != 200) {
        _snprintf(t, sizeof(t), "Der Server antwortet mit %d (%s)", status, url);
        fehler(t); return -1;
    }
    /*  HTTP/1.1: manche Server (und Proxys) lehnen 1.0 ab (426, gemessen);
        dann darf der Rumpf "chunked" kommen.  */
    if (ist_chunked(b, kopf)) {
        DWORD neu = entstueckle(b + kopf, *n - kopf);
        if (neu == 0xFFFFFFFF) { fehler("Antwort in Stuecken (chunked) ist unvollstaendig"); return -1; }
        *n = kopf + neu;
    }
    *rumpf = kopf;
    return 0;
}

int main(int argc, char** argv)
{
    WSADATA wd; WOLFSSL_CTX* ctx; char url[2048], ca[MAX_PATH + 16], t[600];
    char* b; DWORD n = 0, rumpf = 0; int runde, r = -1, k;
    FILE* f;

    if (argc != 3) {
        printf("PCIHOLE -- PCI.IDS ueber HTTPS holen (TLS 1.2, wolfSSL)\n"
               "    PCIHOLE <https://...> <zieldatei>\n"
               "Braucht CACERT.PEM neben dem Programm.\n");
        return 2;
    }
    strncpy(url, argv[1], sizeof(url) - 1); url[sizeof(url) - 1] = 0;

    k = (int)GetModuleFileNameA(NULL, ca, MAX_PATH);
    while (k > 0 && ca[k - 1] != '\\') k--;
    ca[k] = 0; strcat(ca, "CACERT.PEM");

    if (WSAStartup(MAKEWORD(2, 0), &wd) != 0) { fehler("WinSock 2 (ws2_32.dll) laesst sich nicht starten -- ist TCP/IP eingerichtet?"); return 1; }
    wolfSSL_Init();
    ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
    if (!ctx) { fehler("wolfSSL: kein Kontext"); return 1; }
    if (wolfSSL_CTX_load_verify_locations_ex(ctx, ca, NULL, WOLFSSL_LOAD_FLAG_IGNORE_ERR) != WOLFSSL_SUCCESS) {
        _snprintf(t, sizeof(t), "Zertifikatsliste fehlt oder ist unlesbar: %s  -- ohne sie wird "
                  "nicht geladen (sonst waere HTTPS wertlos).", ca);
        fehler(t); return 1;
    }
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, NULL);

    b = (char*)malloc(MAXGROESSE + 1);
    if (!b) { fehler("Kein Speicher"); return 1; }
    for (runde = 0; runde < 5; runde++) {
        r = abruf(ctx, url, sizeof(url), b, &n, &rumpf);
        if (r != 1) break;
    }
    if (r == 1) fehler("Zu viele Umleitungen");
    if (r == 0) {
        f = fopen(argv[2], "wb");
        if (!f) { _snprintf(t, sizeof(t), "Zieldatei laesst sich nicht anlegen: %s", argv[2]); fehler(t); r = -1; }
        else {
            if (fwrite(b + rumpf, 1, n - rumpf, f) != n - rumpf) { fehler("Schreiben fehlgeschlagen"); r = -1; }
            fclose(f);
            if (r == 0) { printf("OK %lu\n", (unsigned long)(n - rumpf)); fflush(stdout); }
        }
    }
    free(b);
    wolfSSL_CTX_free(ctx); wolfSSL_Cleanup(); WSACleanup();
    return r == 0 ? 0 : 1;
}
