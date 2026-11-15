/*
 *  nt/ntunterlage.c -- die Unterlage.
 *
 *  Der FreeBSD-USB-Quelltext darueber bleibt Zeile fuer Zeile unberuehrt.
 *  Hier steht alles, was ihm der Kern schuldet, und sonst nichts.
 *
 *  Gemessen wurden 127 Namen an dieser Kante.  Der groesste Teil davon
 *  sind Einzeiler in den Kopfdateien; was hier liegt, sind die zwoelf
 *  Stellen, an denen die beiden Systeme wirklich verschieden denken.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sx.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/proc.h>
#include <machine/bus.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>


/* ====================================================================
 *  Ausgabe
 *
 *  KiSerialPutString ist der 1017. Export dieses Kernels.  Er schreibt
 *  auf COM1 ohne Kerneldebugger -- fuer einen Treiber, der noch nicht
 *  laeuft, ist das der einzige Weg, der immer funktioniert.
 * ==================================================================== */

const int          hz    = NTU_HZ;
volatile int       ticks = 0;    /* laeuft im Taktgeber-DPC hoch */

static CHAR ntu_buf[512];       /* nicht auf dem Stapel: siehe unten */
static KSPIN_LOCK ntu_buf_lock;

/*  Ein Puffer ueber vier Kilobyte auf dem Stapel laesst clang-cl
 *  __chkstk rufen, und das gibt es unter /nodefaultlib nicht.  Der
 *  Puffer ist deshalb statisch und unter einem Spinlock -- 512 Byte
 *  waeren zwar unkritisch, aber die Gewohnheit ist es wert.  */

#if !defined(NTU_KISERIAL) && !defined(NTU_OHNE_SERIELL)

/* ====================================================================
 *  Eigener UART-Schreiber
 *
 *  16550 auf COM1.  Abgefragt, nicht per Unterbrechung -- die Ausgabe
 *  muss auch aus dem ISR heraus und vor jeder Initialisierung gehen.
 *
 *  Der Baustein wird einmal auf 115200 8N1 gesetzt.  Hat serial.sys
 *  den Anschluss schon in Betrieb, stimmt seine Einstellung womoeglich
 *  nicht mit unserer; fuer einen Pruefbau ist das in Kauf zu nehmen --
 *  und unter QEMU ist die Baudrate ohnehin belanglos.
 *
 *  Gewartet wird hoechstens eine begrenzte Zahl Runden auf THRE.  Fehlt
 *  der Baustein, liest das Register 0xFF, und THRE scheint immer frei;
 *  haengt er, bricht die Schleife ab.  Ein Treiber, der wegen seiner
 *  Diagnoseausgabe haengt, waere die schlechteste aller Moeglichkeiten.
 * ==================================================================== */

/*  Wohin die Ausgabe geht, steht in der Registrierung:
 *
 *      Services\<dienst>\Parameters\Ausgabe   REG_DWORD
 *          0      aus
 *          1      COM1, Port 0x3F8   (Vorgabe, wenn der Wert fehlt)
 *          2      COM2, Port 0x2F8
 *          sonst  die Portadresse selbst
 *
 *  Warum ueberhaupt einstellbar: auf einem NT-4.0-Rechner haengt an COM1
 *  oft eine serielle Maus.  Der Schreiber stellt den Baustein auf 115200
 *  Baud -- die Maus waere beim Booten tot.  Der Installer setzt den Wert
 *  und warnt, wenn Sermouse eingerichtet ist.  */

ULONG ntu_uart_basis = 0x3F8;

/*  Diagnose im Textmodus-Setup (nur Bauart NTU_SETUP_BILDSCHIRM):
 *  jede Zeile zusaetzlich mit HalDisplayString auf den blauen
 *  Startbildschirm -- fuer Rechner ohne COM-Anschluss.  */
ULONG ntu_bildschirm;
static ULONG ntu_bildschirm_zeilen;
static LONGLONG ntu_bildschirm_t0;
NTHALAPI VOID NTAPI HalDisplayString (PUCHAR s);

#define NTU_UART        ((PUCHAR)(ULONG_PTR)ntu_uart_basis)
#define NTU_UART_LSR    ((PUCHAR)(ULONG_PTR)(ntu_uart_basis + 5))
#define NTU_UART_THRE   0x20

static BOOLEAN ntu_uart_bereit;

static VOID
ntu_uart_init (VOID)
{
    WRITE_PORT_UCHAR(NTU_UART + 1, 0x00);       /* keine Unterbrechung */
    WRITE_PORT_UCHAR(NTU_UART + 3, 0x80);       /* DLAB               */
    WRITE_PORT_UCHAR(NTU_UART + 0, 0x01);       /* 115200             */
    WRITE_PORT_UCHAR(NTU_UART + 1, 0x00);
    WRITE_PORT_UCHAR(NTU_UART + 3, 0x03);       /* 8N1                */
    WRITE_PORT_UCHAR(NTU_UART + 2, 0xC7);       /* FIFO an, geleert   */
    WRITE_PORT_UCHAR(NTU_UART + 4, 0x03);       /* DTR RTS            */
    ntu_uart_bereit = TRUE;
}

/*  Hoechstens ~3000 Abfragen je Zeichen.  Auf echter Hardware dauert eine
 *  Abfrage des LSR rund eine Mikrosekunde -- 100000 Runden waren bis zu
 *  0,1 s je Zeichen und damit Sekunden je Zeile, wenn der Baustein
 *  langsamer laeuft (serial.sys stellt ihn z. B. beim Suchen nach einer
 *  seriellen Maus um) oder nie "frei" meldet.  Geschrieben wird aus dem
 *  Eingabeweg heraus: eine Zeile je Mausbericht hielt so jeden Klick
 *  Sekunden auf, waehrend das Laempchen in der Systemsteuerung (vor der
 *  Ausgabe gesetzt) sofort leuchtete.  Bei 115200 Baud braucht ein Zeichen
 *  ~90 Runden; reicht die Grenze nicht, gehen Zeichen verloren -- eine
 *  Diagnoseausgabe darf den Treiber nie aufhalten.  */

static VOID
ntu_uart_zeichen (UCHAR c)
{
    ULONG runden = 3000;

    while ((READ_PORT_UCHAR(NTU_UART_LSR) & NTU_UART_THRE) == 0 &&
           runden != 0) {
        runden -= 1;
    }

    WRITE_PORT_UCHAR(NTU_UART, c);

#if defined(NTU_UART_BREMSE)
    /*  Nur fuer den Pruefstand: 1 ms je Zeichen, wie ein langsamer
     *  Anschluss auf echter Hardware.  Unter QEMU ist die Ausgabe sonst
     *  so schnell, dass Fehler im Umgang mit ihr nie auffallen.  */
    /*  Nach dem Leistungszaehler, nicht mit KeStallExecutionProcessor:
     *  dessen Eichung stimmt unter QEMU nicht, 1000 us waren dort weit
     *  weniger als 1 ms (1320 Zeilen zu 90 Zeichen in Sekunden).  */
    {
        LARGE_INTEGER f, a, b;
        a = KeQueryPerformanceCounter(&f);
        do {
            b = KeQueryPerformanceCounter(NULL);
        } while (b.QuadPart - a.QuadPart < f.QuadPart / 1000);
    }
#endif
}

VOID
ntu_seriell_aus (PCHAR s)
{
    if (ntu_uart_basis == 0) {
        return;                     /* Ausgabe abgeschaltet */
    }

    if (!ntu_uart_bereit) {
        ntu_uart_init();
    }

    while (*s != '\0') {

        if (*s == '\n') {
            ntu_uart_zeichen('\r');
        }

        ntu_uart_zeichen((UCHAR)*s);
        s += 1;
    }
}

/* ====================================================================
 *  Ringpuffer fuer die Ausgabe
 *
 *  Frueher schrieb ntu_printf die Zeile sofort auf den Anschluss -- unter
 *  ntu_buf_lock, also auf DISPATCH_LEVEL, Zeichen fuer Zeichen.  Aus dem
 *  Eingabeweg heraus (eine Zeile je Mausbericht im Diagnose- und frueher
 *  im Herzschlag-Bau) hielt das jeden Klick auf echter Hardware Sekunden
 *  auf; unter Mehrprozessor-Kernen wartete ausserdem jeder andere
 *  Prozessor, der etwas ausgeben wollte.
 *
 *  Jetzt: ntu_printf kopiert die Zeile nur in den Ring (Mikrosekunden)
 *  und weckt ntu_ring_faden.  Der Faden laeuft mit Prioritaet 4, unter
 *  gewoehnlichen Programmen, und schreibt OHNE Sperre.  Ist der Ring voll,
 *  wird die Zeile gezaehlt statt geschrieben; spaeter erscheint
 *  "N Zeilen verloren".  Vor dem Start des Fadens (fruehes DriverEntry)
 *  und mit -DNTU_OHNE_RING wird wie frueher direkt geschrieben.
 *
 *  Beim Bluescreen leert ein Rueckruf (KeRegisterBugCheckCallback) den
 *  Ring direkt -- gerade die letzten Zeilen braucht man dann.
 * ==================================================================== */

#define NTU_RING_ZEILEN  256
#define NTU_RING_BREITE  200

static CHAR          ntu_ring[NTU_RING_ZEILEN][NTU_RING_BREITE];
static ULONG         ntu_ring_kopf, ntu_ring_fuss;     /* unter ntu_buf_lock */
static ULONG         ntu_ring_verloren;                /* unter ntu_buf_lock */
static volatile LONG ntu_ring_an;
static volatile LONG ntu_ring_ende;
static KEVENT        ntu_ring_ereignis;
static PVOID         ntu_ring_faden_obj;
static KBUGCHECK_CALLBACK_RECORD ntu_ring_absturz_satz;
static BOOLEAN       ntu_ring_absturz_da;

/*  In den Ring legen.  Unter ntu_buf_lock.  TRUE, wenn gelegt.  */

static BOOLEAN
ntu_ring_legen (const CHAR *s)
{
    CHAR *z;
    ULONG i;

    if (ntu_ring_kopf - ntu_ring_fuss >= NTU_RING_ZEILEN) {
        ntu_ring_verloren += 1;
        return FALSE;
    }
    z = ntu_ring[ntu_ring_kopf % NTU_RING_ZEILEN];
    for (i = 0; s[i] != '\0' && i < NTU_RING_BREITE - 1; i += 1) {
        z[i] = s[i];
    }
    z[i] = '\0';
    ntu_ring_kopf += 1;
    return TRUE;
}

/*  Alles Anstehende schreiben.  Nur aus ntu_ring_faden (PASSIVE_LEVEL).  */

static VOID
ntu_ring_leeren (VOID)
{
    static CHAR zeile[NTU_RING_BREITE];
    CHAR        v[80];
    KIRQL       alt;
    ULONG       verl, i;

    for (;;) {
        KeAcquireSpinLock(&ntu_buf_lock, &alt);
        if (ntu_ring_fuss == ntu_ring_kopf && ntu_ring_verloren == 0) {
            KeReleaseSpinLock(&ntu_buf_lock, alt);
            return;
        }
        verl = ntu_ring_verloren;
        ntu_ring_verloren = 0;
        zeile[0] = '\0';
        if (ntu_ring_fuss != ntu_ring_kopf) {
            CHAR *q = ntu_ring[ntu_ring_fuss % NTU_RING_ZEILEN];
            for (i = 0; i < NTU_RING_BREITE; i += 1) {
                zeile[i] = q[i];
            }
            ntu_ring_fuss += 1;
        }
        KeReleaseSpinLock(&ntu_buf_lock, alt);

        if (verl != 0) {
            _snprintf(v, sizeof(v) - 1, "usb: %lu Zeilen verloren (Ausgabe zu langsam)\n",
                      (unsigned long)verl);
            v[sizeof(v) - 1] = '\0';
            ntu_seriell_aus(v);
        }
        if (zeile[0] != '\0') {
            ntu_seriell_aus(zeile);
        }
    }
}

static VOID
ntu_ring_faden (PVOID p)
{
    LARGE_INTEGER t;

    UNREFERENCED_PARAMETER(p);
    KeSetPriorityThread(KeGetCurrentThread(), 4);

    while (!ntu_ring_ende) {
        t.QuadPart = -100 * 10000;              /* 100 ms, falls ein Wecken fehlt */
        KeWaitForSingleObject(&ntu_ring_ereignis, Executive, KernelMode, FALSE, &t);
        ntu_ring_leeren();
    }
    ntu_ring_leeren();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/*  Bluescreen: den Ring direkt hinausschreiben, ohne Sperre (HIGH_LEVEL,
 *  nichts anderes laeuft mehr).  Auch aus ntu_panic.  */

static VOID
ntu_ring_absturz (PVOID puffer, ULONG laenge)
{
    UNREFERENCED_PARAMETER(puffer);
    UNREFERENCED_PARAMETER(laenge);

    while (ntu_ring_fuss != ntu_ring_kopf) {
        ntu_seriell_aus(ntu_ring[ntu_ring_fuss % NTU_RING_ZEILEN]);
        ntu_ring_fuss += 1;
    }
}

VOID
ntu_ring_starten (VOID)
{
#if !defined(NTU_OHNE_RING)
    HANDLE   h;
    NTSTATUS st;

    if (ntu_uart_basis == 0 || ntu_ring_an) {
        return;                 /* Ausgabe aus: nichts zu puffern */
    }
    KeInitializeEvent(&ntu_ring_ereignis, SynchronizationEvent, FALSE);
    ntu_ring_ende = 0;
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                              ntu_ring_faden, NULL);
    if (!NT_SUCCESS(st)) {
        return;                 /* dann eben weiter direkt */
    }
    ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, NULL, KernelMode,
                              &ntu_ring_faden_obj, NULL);
    ZwClose(h);

    KeInitializeCallbackRecord(&ntu_ring_absturz_satz);
    ntu_ring_absturz_da = KeRegisterBugCheckCallback(&ntu_ring_absturz_satz,
                              ntu_ring_absturz, ntu_ring, sizeof(ntu_ring),
                              (PUCHAR)"usbnt");
    InterlockedExchange((LONG *)&ntu_ring_an, 1);
#endif
}

/*  Beim Entladen: erst alles hinaus, dann den Faden beenden.  */

VOID
ntu_ring_anhalten (VOID)
{
    if (!ntu_ring_an) {
        return;
    }
    InterlockedExchange((LONG *)&ntu_ring_an, 0);   /* ab jetzt wieder direkt */
    InterlockedExchange((LONG *)&ntu_ring_ende, 1);
    KeSetEvent(&ntu_ring_ereignis, 0, FALSE);
    if (ntu_ring_faden_obj != NULL) {
        KeWaitForSingleObject(ntu_ring_faden_obj, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(ntu_ring_faden_obj);
        ntu_ring_faden_obj = NULL;
    }
    if (ntu_ring_absturz_da) {
        KeDeregisterBugCheckCallback(&ntu_ring_absturz_satz);
        ntu_ring_absturz_da = FALSE;
    }
}

#endif

VOID
ntu_printf (const char *fmt, ...)
{
    va_list ap;
    KIRQL   old;

#if !defined(NTU_KISERIAL) && !defined(NTU_OHNE_SERIELL)
    BOOLEAN wecken = FALSE;
#endif

    KeAcquireSpinLock(&ntu_buf_lock, &old);
    va_start(ap, fmt);
    _vsnprintf(ntu_buf, sizeof(ntu_buf) - 1, fmt, ap);
    va_end(ap);
    ntu_buf[sizeof(ntu_buf) - 1] = '\0';
    if (ntu_bildschirm && ntu_bildschirm_zeilen < 400) {
        static CHAR z[300];
        LARGE_INTEGER f, t = KeQueryPerformanceCounter(&f);
        ULONG ms;
        if (ntu_bildschirm_t0 == 0) {
            ntu_bildschirm_t0 = t.QuadPart;
        }
        ms = (ULONG)((t.QuadPart - ntu_bildschirm_t0) * 1000 / (f.QuadPart ? f.QuadPart : 1));
        ntu_bildschirm_zeilen += 1;
        _snprintf(z, sizeof(z) - 1, "[%6u] %s", ms,
                  ntu_buf[0] == '\r' ? ntu_buf + 2 : ntu_buf);
        z[sizeof(z) - 1] = '\0';
        HalDisplayString((PUCHAR)z);
    }
#if !defined(NTU_KISERIAL) && !defined(NTU_OHNE_SERIELL)
    if (ntu_ring_an) {
        wecken = ntu_ring_legen(ntu_buf);       /* Mikrosekunden */
    } else {
        KiSerialPutString(ntu_buf);             /* vor dem Faden: direkt */
    }
#else
    KiSerialPutString(ntu_buf);
#endif
    KeReleaseSpinLock(&ntu_buf_lock, old);
#if !defined(NTU_KISERIAL) && !defined(NTU_OHNE_SERIELL)
    if (wecken) {
        KeSetEvent(&ntu_ring_ereignis, 0, FALSE);
    }
#endif
}

VOID
ntu_panic (const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(ntu_buf, sizeof(ntu_buf) - 1, fmt, ap);
    va_end(ap);
#if !defined(NTU_KISERIAL) && !defined(NTU_OHNE_SERIELL)
    ntu_ring_absturz(0, 0);         /* was noch im Ring steht, zuerst */
#endif
    KiSerialPutString("\r\nUSB-PANIK: ");
    KiSerialPutString(ntu_buf);
    KiSerialPutString("\r\n");
    KeBugCheckEx(0xDEAD0055, 0, 0, 0, 0);
}


/* ====================================================================
 *  Speicher
 *
 *  M_WAITOK heisst "darf schlafen".  NT schlaeft nicht, es liefert NULL.
 *  Das ist kein Problem, weil der USB-Stapel jede Rueckgabe prueft --
 *  nachgesehen in usb_alloc_device, usbd_transfer_setup und
 *  usb_hub_explore, nicht angenommen.
 *
 *  Alles aus NonPagedPool.  PagedPool waere ein Bugcheck mit Ansage.
 *
 *  ------------------------------------------------------------------
 *  Die Wache
 *
 *  Vor jeden Block kommt ein Kopf, hinter jeden ein Endzeichen.  Damit
 *  faengt die Unterlage die drei Fehler ab, die sonst alle im selben
 *  Bugcheck enden -- MiFreePoolPages, und der Abzug zeigt den Kern
 *  statt den Treiber, der ihn falsch gerufen hat:
 *
 *      fremder Zeiger     Kennzeichen stimmt nicht
 *      doppelte Freigabe  Kennzeichen wurde beim ersten Mal geloescht
 *      Ueberlauf          Endzeichen ueberschrieben
 *
 *  In allen drei Faellen wird NICHT freigegeben.  Ein Block zuviel im
 *  Speicher ist harmlos; ein falscher Zeiger an ExFreePool ist es nicht.
 *  Stattdessen steht eine Zeile im Bericht, mit Groesse und laufender
 *  Nummer der Vergabe -- damit laesst sich die Stelle finden.
 *
 *  Der Kopf ist sechzehn Byte, die Ausrichtung bleibt also bei acht.
 *  Fuer DMA-Puffer spielt das ohnehin keine Rolle: die kommen aus
 *  HalAllocateCommonBuffer und nicht von hier.
 *
 *  Abschaltbar mit -DNTU_OHNE_POOLWACHE, sobald es laeuft.  Sie kostet
 *  sechzehn Byte je Block und zwei Vergleiche je Freigabe.
 * ==================================================================== */

#ifndef NTU_OHNE_POOLWACHE

#define NTU_KOPF_MAGIE   0x42535504ul      /* "\004USB" */
#define NTU_TOT_MAGIE    0x44454144ul      /* "DEAD"     */
#define NTU_ENDE_MAGIE   0x454e4445ul      /* "ENDE"     */

typedef struct ntu_kopf {
    ULONG Magie;
    ULONG Groesse;
    ULONG Nummer;
    ULONG Fuell;
} ntu_kopf;

static ULONG ntu_vergaben;
static LONG  ntu_offen;
static ULONG ntu_gemeckert;

ULONG ntu_pool_offen(VOID)    { return (ULONG)ntu_offen; }
ULONG ntu_pool_vergaben(VOID) { return ntu_vergaben; }
ULONG ntu_pool_gemeckert(VOID) { return ntu_gemeckert; }

void *
ntu_malloc (size_t size, int flags)
{
    ntu_kopf *k;
    UCHAR    *p;

    if (size == 0) {
        size = 1;
    }

    k = (ntu_kopf *)ExAllocatePoolWithTag(NonPagedPool,
                                          (ULONG)size + sizeof(ntu_kopf) + 4,
                                          NTU_TAG);

    if (k == 0) {
        return 0;
    }

    k->Magie   = NTU_KOPF_MAGIE;
    k->Groesse = (ULONG)size;
    k->Fuell   = 0;

    /*  Atomar: unter einem Mehrprozessor-Kern vergeben zwei Prozessoren
     *  gleichzeitig, und "+= 1" verlore dann Schritte -- die Leckpruefung
     *  (offen vorher = offen nachher) waere unzuverlaessig.  */

    k->Nummer = (ULONG)InterlockedIncrement((LONG *)&ntu_vergaben) - 1;
    InterlockedIncrement(&ntu_offen);

    p = (UCHAR *)(k + 1);

    /*  Endzeichen hinter die Nutzdaten.  Byteweise, weil size beliebig
     *  ist und der Block dahinter nicht ausgerichtet sein muss.  */

    p[size + 0] = (UCHAR)(NTU_ENDE_MAGIE >> 24);
    p[size + 1] = (UCHAR)(NTU_ENDE_MAGIE >> 16);
    p[size + 2] = (UCHAR)(NTU_ENDE_MAGIE >>  8);
    p[size + 3] = (UCHAR)(NTU_ENDE_MAGIE >>  0);

    if ((flags & M_ZERO) != 0) {
        memset(p, 0, size);
    }

    return p;
}

void
ntu_free (void *v)
{
    ntu_kopf *k;
    UCHAR    *p = (UCHAR *)v;
    ULONG     ende;

    if (v == 0) {
        return;
    }

    k = ((ntu_kopf *)v) - 1;

    if (k->Magie == NTU_TOT_MAGIE) {

        /*  Schon einmal freigegeben.  Nummer und Groesse stehen noch im
         *  Kopf, weil wir ihn nicht ueberschreiben -- genau dafuer.  */

        ntu_printf("usb: POOL doppelte Freigabe, Vergabe %u, %u Byte\n",
                   k->Nummer, k->Groesse);
        InterlockedIncrement((LONG *)&ntu_gemeckert);
        return;
    }

    if (k->Magie != NTU_KOPF_MAGIE) {

        /*  Kein Block von uns.  Hier waere ExFreePool der Bugcheck in
         *  MiFreePoolPages gewesen.  */

        ntu_printf("usb: POOL fremder Zeiger %p, Kopf %08x\n",
                   v, k->Magie);
        InterlockedIncrement((LONG *)&ntu_gemeckert);
        return;
    }

    ende = ((ULONG)p[k->Groesse + 0] << 24) | ((ULONG)p[k->Groesse + 1] << 16)
         | ((ULONG)p[k->Groesse + 2] <<  8) | ((ULONG)p[k->Groesse + 3]);

    if (ende != NTU_ENDE_MAGIE) {

        /*  Ueberlauf.  Der Block wird trotzdem nicht freigegeben: was
         *  ueber sein Ende hinausgeschrieben hat, hat womoeglich den
         *  Poolkopf des NACHFOLGERS erwischt, und dann stirbt die
         *  naechste fremde Freigabe.  */

        ntu_printf("usb: POOL Ueberlauf, Vergabe %u, %u Byte, "
                   "Endzeichen %08x\n", k->Nummer, k->Groesse, ende);
        InterlockedIncrement((LONG *)&ntu_gemeckert);
        return;
    }

    k->Magie = NTU_TOT_MAGIE;
    InterlockedDecrement(&ntu_offen);
    ExFreePool(k);
}

#else   /* NTU_OHNE_POOLWACHE */

ULONG ntu_pool_offen(VOID)    { return 0; }
ULONG ntu_pool_vergaben(VOID) { return 0; }
ULONG ntu_pool_gemeckert(VOID) { return 0; }

void *
ntu_malloc (size_t size, int flags)
{
    void *p;

    if (size == 0) {
        size = 1;
    }

    p = ExAllocatePoolWithTag(NonPagedPool, (ULONG)size, NTU_TAG);

    if (p != 0 && (flags & M_ZERO) != 0) {
        memset(p, 0, size);
    }

    return p;
}

void
ntu_free (void *p)
{
    if (p != 0) {
        ExFreePool(p);
    }
}

#endif

/* ====================================================================
 *  Sperren
 *
 *  mtx wird KMUTEX, nicht KSPIN_LOCK.  Das ist die wichtigste einzelne
 *  Entscheidung dieser Unterlage, und sie folgt aus dem Treiber, nicht
 *  aus dem Geschmack:
 *
 *      usbd_transfer_drain   cv_wait unter gehaltenem mtx
 *      usb_proc_mwait        dito
 *      usb_hub_explore       nimmt mtx ueber Millisekunden
 *
 *  Unter einem Spinlock waere jede dieser Stellen ein Bugcheck.  Der
 *  Preis ist, dass alles, was mtx nimmt, auf PASSIVE_LEVEL laufen muss
 *  -- und genau dafuer ist der Fadenumbau der Unterbrechung da.
 * ==================================================================== */

VOID
ntu_mtx_init (ntu_mtx *m, const char *name, const char *type, int opts)
{
    /*  MTX_SPIN wird hier NICHT zum Spinlock.
     *
     *  Die erste Fassung tat das und legte die alte IRQL im Mutex ab.
     *  usb_controller.c legt usb_spin_mtx als MTX_SPIN | MTX_RECURSE an;
     *  beim zweiten, rekursiven Nehmen ueberschreibt KeAcquireSpinLock
     *  die gemerkte IRQL mit DISPATCH_LEVEL, und das erste Freigeben
     *  stellt dann DISPATCH_LEVEL wieder her statt PASSIVE.  Ab da
     *  laeuft das ganze System auf IRQL 2 weiter.
     *
     *  Der Absturz kommt dann irgendwann und irgendwo -- bei uns in
     *  MmCreateSectionReal, weil dort als naechstes jemand einen PTE
     *  lesen wollte.  Im Abzug steht Mm und kein Wort von USB.
     *
     *  Ein Spinlock ist hier ohnehin nicht noetig: der Fadenumbau der
     *  Unterbrechung bringt alles auf PASSIVE_LEVEL, es gibt also
     *  keinen Rufer auf DISPATCH_LEVEL, gegen den gesperrt werden
     *  muesste.  Und KMUTEX ist von sich aus rekursiv, womit
     *  MTX_RECURSE ohne Zutun stimmt.  */

    memset(m, 0, sizeof(*m));
    m->Name = name;
    m->Spin = 0;
    KeInitializeMutex(&m->Mutex, 0);
}

VOID
ntu_mtx_destroy (ntu_mtx *m)
{
    m->Name = 0;
}

/* ====================================================================
 *  Wartetabelle
 *
 *  Die Plaetze werden unter einer Drehsperre vergeben; ein Faden haelt
 *  seinen Platz, solange es ihn gibt, und schreibt danach nur noch in
 *  den eigenen -- ohne Sperre, weil niemand sonst hineinschreibt.
 * ==================================================================== */

ntu_wart   ntu_wart_tab[NTU_WART_MAX];
static KSPIN_LOCK ntu_wart_sperre;
static int        ntu_wart_bereit;

static ntu_wart *
ntu_wart_platz (VOID)
{
    PKTHREAD  ich = KeGetCurrentThread();
    ntu_wart *frei = 0;
    KIRQL     alt;
    int       k;

    if (!ntu_wart_bereit) {
        KeInitializeSpinLock(&ntu_wart_sperre);
        ntu_wart_bereit = 1;
    }

    for (k = 0; k < NTU_WART_MAX; k += 1) {
        if (ntu_wart_tab[k].Faden == ich) {
            return &ntu_wart_tab[k];
        }
    }

    KeAcquireSpinLock(&ntu_wart_sperre, &alt);

    for (k = 0; k < NTU_WART_MAX; k += 1) {
        if (ntu_wart_tab[k].Faden == 0) {
            ntu_wart_tab[k].Faden = ich;
            frei = &ntu_wart_tab[k];
            break;
        }
    }

    KeReleaseSpinLock(&ntu_wart_sperre, alt);
    return frei;              /* 0, wenn die Tabelle voll ist: dann eben
                                 ohne Eintrag -- nie deshalb scheitern */
}

VOID
ntu_wart_name (const char *name)
{
    ntu_wart *w = ntu_wart_platz();

    if (w != 0) {
        w->FadenName = name;
    }
}

VOID
ntu_wart_rein (const char *art, const char *worauf, PKTHREAD halter)
{
    ntu_wart *w;

    /*  Nur auf PASSIVE_LEVEL eintragen: ein DPC oder ISR wartet ohnehin
     *  nicht, und KeGetCurrentThread waere dort der unterbrochene Faden.  */

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    w = ntu_wart_platz();

    if (w != 0) {
        w->Worauf = worauf;
        w->Halter = halter;
        w->Seit   = ticks;
        w->Art    = art;
    }
}

VOID
ntu_wart_raus (VOID)
{
    ntu_wart *w;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    w = ntu_wart_platz();

    if (w != 0) {
        w->Art = 0;
    }
}

VOID
ntu_mtx_lock (ntu_mtx *m)
{
    /*  KMUTEX ist von sich aus rekursiv; FreeBSD verlangt das bei
     *  MTX_RECURSE und verbietet es sonst.  Wir erlauben es durchweg und
     *  zaehlen mit, damit mtx_owned stimmt.  Ein fehlendes Verbot ist
     *  hier die sichere Richtung: es kann nichts verklemmen, was unter
     *  FreeBSD gelaufen waere.  */

    ntu_wart_rein("mtx", m->Name, m->Owner);
    KeWaitForSingleObject(&m->Mutex, Executive, KernelMode, FALSE, NULL);
    ntu_wart_raus();
    m->Owner = KeGetCurrentThread();
    m->Depth += 1;
}

VOID
ntu_mtx_unlock (ntu_mtx *m)
{
    m->Depth -= 1;

    if (m->Depth == 0) {
        m->Owner = 0;
    }

    KeReleaseMutex(&m->Mutex, FALSE);
}

int
ntu_mtx_owned (ntu_mtx *m)
{
    return (m->Owner == KeGetCurrentThread());
}


/* ====================================================================
 *  Bedingungsvariablen
 *
 *  Hier liegt das einzige echte Fenster der ganzen Unterlage, und es ist
 *  ausdruecklich beschrieben statt stillschweigend in Kauf genommen.
 *
 *  FreeBSDs cv_wait gibt den Mutex ab, wartet und nimmt ihn wieder -- in
 *  einem Zug, vom Planer garantiert.  KeWaitForSingleObject kann das
 *  nicht; zwischen Abgeben und Warten liegt ein Fenster.
 *
 *  Es ist unschaedlich, weil ein KEVENT vom Typ SynchronizationEvent den
 *  Weckruf festhaelt: wer im Fenster geweckt wird, findet das Ereignis
 *  gesetzt vor und kehrt sofort zurueck.
 *
 *  Zwei Dinge folgen daraus:
 *
 *    1.  cv_signal und cv_broadcast werden in FreeBSD immer unter
 *        gehaltenem Mutex gerufen.  Waiters ist damit geschuetzt und
 *        broadcast kann genau so oft wecken, wie jemand wartet.
 *
 *    2.  Ein Weckruf ohne Wartenden bleibt stehen und laesst das
 *        naechste cv_wait sofort zurueckkehren -- ein falscher Weckruf.
 *        Jeder Rufer im USB-Stapel prueft danach seine Bedingung erneut
 *        in einer Schleife; nachgesehen, nicht angenommen.
 * ==================================================================== */

VOID
ntu_cv_init (ntu_cv *c, const char *desc)
{
    memset(c, 0, sizeof(*c));
    c->Name = desc;
    KeInitializeEvent(&c->Event, SynchronizationEvent, FALSE);
}

VOID
ntu_cv_destroy (ntu_cv *c)
{
    c->Name = 0;
}

VOID
ntu_cv_wait (ntu_cv *c, ntu_mtx *m)
{
    c->Waiters += 1;
    ntu_mtx_unlock(m);
    ntu_wart_rein("cv", c->Name, 0);
    KeWaitForSingleObject(&c->Event, Executive, KernelMode, FALSE, NULL);
    ntu_wart_raus();
    ntu_mtx_lock(m);
    c->Waiters -= 1;
}

int
ntu_cv_timedwait (ntu_cv *c, ntu_mtx *m, int timo)
{
    LARGE_INTEGER to;
    NTSTATUS      st;

    /*  hz ist 1000, ein Tick also eine Millisekunde.  Negativ heisst
     *  relativ.  Die Multiplikation bleibt 32 Bit, bis sie in den
     *  LARGE_INTEGER geht -- keine 64-Bit-Arithmetik im Treibercode.  */

    if (timo < 0) {
        timo = 0;
    }

    to.QuadPart = -((LONGLONG)(ULONG)timo * NTU_TICK_100NS);

    c->Waiters += 1;
    ntu_mtx_unlock(m);
    ntu_wart_rein("cv/zeit", c->Name, 0);
    st = KeWaitForSingleObject(&c->Event, Executive, KernelMode, FALSE, &to);
    ntu_wart_raus();
    ntu_mtx_lock(m);
    c->Waiters -= 1;

    return (st == STATUS_TIMEOUT) ? EWOULDBLOCK : 0;
}

VOID
ntu_cv_signal (ntu_cv *c)
{
    KeSetEvent(&c->Event, 0, FALSE);
}

VOID
ntu_cv_broadcast (ntu_cv *c)
{
    ULONG i;
    ULONG n = c->Waiters;

    /*  SynchronizationEvent weckt genau einen.  Fuer broadcast also so
     *  oft setzen, wie jemand wartet.  Waiters steht unter dem Mutex des
     *  Rufers, ist hier also stabil.  */

    for (i = 0; i < n; i += 1) {
        KeSetEvent(&c->Event, 0, FALSE);
    }
}


/* ====================================================================
 *  Register
 *
 *  Zwei Adressraeume, ein Zweig.  UHCI liegt im E/A-Raum, OHCI, EHCI und
 *  xHCI liegen im Speicher; welcher es ist, steht beim Anhaengen fest
 *  und aendert sich nie.
 *
 *  READ_REGISTER_* kommt aus NTOSKRNL, READ_PORT_* aus der HAL.  Beides
 *  gegen tools/ntkrnlmp.def geprueft: die Port-Formen fehlen dort, wie
 *  es sein muss.
 * ==================================================================== */

uint8_t
ntu_r1 (bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
    return _NTU_IO(t) ? READ_PORT_UCHAR((PUCHAR)(h + o))
                      : READ_REGISTER_UCHAR((PUCHAR)(h + o));
}

uint16_t
ntu_r2 (bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
    return _NTU_IO(t) ? READ_PORT_USHORT((PUSHORT)(h + o))
                      : READ_REGISTER_USHORT((PUSHORT)(h + o));
}

uint32_t
ntu_r4 (bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
    return _NTU_IO(t) ? READ_PORT_ULONG((PULONG)(h + o))
                      : READ_REGISTER_ULONG((PULONG)(h + o));
}

void
ntu_w1 (bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, uint8_t v)
{
    if (_NTU_IO(t)) { WRITE_PORT_UCHAR((PUCHAR)(h + o), v); }
    else            { WRITE_REGISTER_UCHAR((PUCHAR)(h + o), v); }
}

void
ntu_w2 (bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, uint16_t v)
{
    if (_NTU_IO(t)) { WRITE_PORT_USHORT((PUSHORT)(h + o), v); }
    else            { WRITE_REGISTER_USHORT((PUSHORT)(h + o), v); }
}

void
ntu_w4 (bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, uint32_t v)
{
    if (_NTU_IO(t)) { WRITE_PORT_ULONG((PULONG)(h + o), v); }
    else            { WRITE_REGISTER_ULONG((PULONG)(h + o), v); }
}


/* ====================================================================
 *  DMA
 *
 *  Der USB-Stapel braucht von bus_dma nur einen Fall: einen Puffer, den
 *  CPU und Baustein gleichzeitig sehen.  Das ist HalAllocateCommonBuffer,
 *  eins zu eins.
 *
 *  Streuung/Sammlung braucht er nicht, weil er in seine eigenen Puffer
 *  kopiert.  bus_dmamap_load meldet deshalb EINVAL, sobald mehr als ein
 *  zusammenhaengendes Stueck verlangt wird -- ein ehrlicher Fehler statt
 *  einer halben Streuliste, die erst auf der Maschine auffaellt.
 *
 *  Zwei Dinge, die bei USB wirklich weh tun:
 *
 *    Ausrichtung.  HalAllocateCommonBuffer liefert seitenausgerichtet.
 *    Das reicht fuer die UHCI-Rahmenliste (4096) und die EHCI-
 *    Periodenliste (4096).  Die HCCA von OHCI braucht 256, die
 *    Deskriptoren 16 oder 32 -- die werden innerhalb einer Seite selbst
 *    geschnitten, nicht einzeln angefordert.
 *
 *    Zwischenspeicher.  CacheEnable = FALSE, immer.  Damit sind
 *    bus_dmamap_sync und die usb_pc_cpu_flush-Rufe des Stapels echte
 *    Nullrufe -- aber sie bleiben stehen.  Das ist die Falle aus dem
 *    Skill: wer sie herausnimmt, weil sie gerade nichts tun, verliert
 *    sie an dem Tag, an dem jemand CacheEnable umstellt.
 * ==================================================================== */

int
bus_dma_tag_create (bus_dma_tag_t parent, bus_size_t align, bus_size_t bnd,
                    bus_addr_t lowaddr, bus_addr_t highaddr, void *ff,
                    void *fa, bus_size_t maxsize, int nseg,
                    bus_size_t maxsegsz, int flags, void *lf, void *la,
                    bus_dma_tag_t *out)
{
    ntu_dma_tag_t t;

    t = (ntu_dma_tag_t)ntu_malloc(sizeof(*t), M_ZERO);

    if (t == 0) {
        return ENOMEM;
    }

    t->Alignment = (align < 1) ? 1 : (ULONG)align;
    t->MaxSize   = (ULONG)maxsize;
    t->NSegments = (nseg < 1) ? 1 : (ULONG)nseg;
    t->Adapter   = (parent != 0) ? parent->Adapter : 0;
    t->Device    = (parent != 0) ? parent->Device  : 0;

    if (t->Adapter == 0) {

        /*  Ein Rahmen ohne Adapter ist nicht brauchbar, und der Fehler
         *  faellt sonst erst beim ersten Puffer auf -- zwei Ebenen
         *  weiter unten, wo niemand mehr weiss, welcher Rahmen gemeint
         *  war.  */

        ntu_printf("usb: bus_dma_tag_create ohne Elternrahmen\n");
        ntu_free(t);
        return ENOMEM;
    }

    *out = t;
    return 0;
}

int
bus_dma_tag_destroy (bus_dma_tag_t t)
{
    ntu_free(t);
    return 0;
}

int
bus_dmamem_alloc (bus_dma_tag_t t, void **vaddr, int flags, bus_dmamap_t *mp)
{
    ntu_dma_map_t m;

    m = (ntu_dma_map_t)ntu_malloc(sizeof(*m), M_ZERO);

    if (m == 0) {
        return ENOMEM;
    }

    m->Tag    = t;
    m->Len    = t->MaxSize;
    m->Cached = FALSE;

    /*  Ohne Adapter gar nicht erst rufen.  Die HAL greift ihren ersten
     *  Parameter ungeprueft an -- ein NULL dort ist ein Bluescreen in
     *  HalAllocateCommonBuffer+0x18, und im Abzug steht dann die HAL und
     *  nicht der Treiber, der sie falsch gerufen hat.  */

    if (t->Adapter == 0) {
        ntu_printf("usb: gemeinsamer Puffer ohne Adapter angefordert -- "
                   "bus_get_dma_tag hat nichts geliefert\n");
        ntu_free(m);
        return ENOMEM;
    }

    /*  HalAllocateCommonBuffer richtet NICHT auf Seitengrenzen aus.
     *  Die erste Fassung nahm das an und scheiterte auf dem ersten Brett,
     *  das es anders machte:
     *
     *      usb: gemeinsamer Puffer nicht auf 4096 ausgerichtet
     *           (phys 009e3008)
     *
     *  Ausgerichtet wird deshalb selbst: mehr anfordern und den Anfang
     *  nach oben schieben.  Weil der Puffer zusammenhaengend ist, gilt
     *  derselbe Versatz fuer die virtuelle und die physische Adresse --
     *  das ist die Eigenschaft, die einen gemeinsamen Puffer ausmacht,
     *  und nur deshalb geht es so einfach.
     *
     *  Zum Freigeben muessen Anfang und Laenge des ECHTEN Blocks
     *  erhalten bleiben; HalFreeCommonBuffer will genau die Werte
     *  zurueck, die HalAllocateCommonBuffer geliefert hat.  */

    /*  Eine Seite Zugabe hinten, immer.
     *
     *  Manche Bausteine holen TRBs ueber das Ende des Rings hinaus --
     *  Linux fuehrt das als XHCI_TRB_OVERFETCH fuer VIA VL805 und
     *  Verwandte, und VL805 sitzt auf sehr vielen billigen Steckkarten.
     *  Liegt der Ring am Ende seines Blocks, liest der Baustein dabei in
     *  fremden Speicher.
     *
     *  Eine zusaetzliche Seite kostet je Ring vier Kilobyte und macht
     *  das Vorablesen harmlos -- ohne dass irgendein Baustein erkannt
     *  werden muss.  Eine Abhilfe, die keine Kennung braucht, kann auch
     *  nicht am falschen Baustein angewandt werden.  */

    m->RohLen = t->MaxSize + ((t->Alignment > 1) ? t->Alignment - 1 : 0)
                           + PAGE_SIZE;
    m->RohVa  = HalAllocateCommonBuffer(t->Adapter, m->RohLen, &m->RohPa,
                                        FALSE);

    if (m->RohVa == 0) {
        ntu_free(m);
        return ENOMEM;
    }

    {
        ULONG versatz = 0;

        if (t->Alignment > 1) {
            versatz = (t->Alignment - (m->RohPa.LowPart & (t->Alignment - 1)))
                        & (t->Alignment - 1);
        }

        m->Va           = (PVOID)((UCHAR *)m->RohVa + versatz);
        m->Pa.LowPart   = m->RohPa.LowPart + versatz;
        m->Pa.HighPart  = m->RohPa.HighPart;
    }

    if (m->Pa.HighPart != 0) {

        /*  Ueber 4 GB.  bus_addr_t ist 32 Bit, siehe sys/types.h -- ein
         *  abgeschnittener Zeiger waere ein Baustein, der in fremden
         *  Speicher schreibt.  */

        ntu_printf("usb: gemeinsamer Puffer ueber 4 GB (%08x%08x)\n",
                   m->Pa.HighPart, m->Pa.LowPart);
        HalFreeCommonBuffer(t->Adapter, m->RohLen, m->RohPa, m->RohVa, FALSE);
        ntu_free(m);
        return ENOMEM;
    }

    if ((flags & BUS_DMA_ZERO) != 0) {
        memset(m->Va, 0, t->MaxSize);
    }

    *vaddr = m->Va;
    *mp    = m;
    return 0;
}

void
bus_dmamem_free (bus_dma_tag_t t, void *va, bus_dmamap_t m)
{
    if (m == 0) {
        return;
    }

    /*  Mit den Werten vom Anfordern, nicht mit den ausgerichteten.  */

    HalFreeCommonBuffer(t->Adapter, m->RohLen, m->RohPa, m->RohVa, FALSE);
    ntu_free(m);
}

int
bus_dmamap_create (bus_dma_tag_t t, int flags, bus_dmamap_t *mp)
{
    ntu_dma_map_t m = (ntu_dma_map_t)ntu_malloc(sizeof(*m), M_ZERO);

    if (m == 0) {
        return ENOMEM;
    }

    m->Tag = t;
    *mp = m;
    return 0;
}

int
bus_dmamap_destroy (bus_dma_tag_t t, bus_dmamap_t m)
{
    ntu_free(m);
    return 0;
}

int
bus_dmamap_load (bus_dma_tag_t t, bus_dmamap_t m, void *buf, bus_size_t len,
                 bus_dmamap_callback_t cb, void *arg, int flags)
{
    bus_dma_segment_t seg[NTU_MAX_SEG];
    PHYSICAL_ADDRESS  pa;
    ULONG             n = 0;
    UCHAR            *p = (UCHAR *)buf;
    ULONG             rest = (ULONG)len;

    /*  Zwei Faelle, und die erste Fassung kannte nur den halben.
     *
     *  1.  Der Puffer stammt aus bus_dmamem_alloc.  Dann ist er per
     *      Bauart zusammenhaengend, und die physische Adresse steht
     *      bereits in der Abbildung.  Eine Seitengrenze darin ist
     *      voellig belanglos.
     *
     *      Die erste Fassung wies genau das ab:
     *
     *          if (len > PAGE_SIZE - (buf & PAGE_MASK)) return EINVAL;
     *
     *      Beim Anhaengen fiel es nicht auf, weil die xHCI-Ringe genau
     *      4096 Byte gross und seitenausgerichtet sind und damit gerade
     *      durchrutschten.  Die Geraetekontexte werden aber erst beim
     *      Aufzaehlen angefordert -- also in dem Augenblick, in dem ein
     *      Anschluss belegt meldet.  Dann scheiterte usb_pc_alloc_mem,
     *      die Aufzaehlung blieb stehen, und am Anschluss stand CCS
     *      ohne PED.
     *
     *  2.  Ein gewoehnlicher Kernpuffer.  Der ist physisch NICHT
     *      zusammenhaengend, also wird er seitenweise zerlegt.  Genau
     *      dafuer hat usb_pc_common_mem_cb den nseg-Zweig, und
     *      pc->ismultiseg sagt dem Stapel, dass er damit rechnen muss.
     *
     *  Das Seitenversatz-Loch, das die Pruefung dort sucht, kann hier
     *  nicht entstehen: wir schneiden an Seitengrenzen, jedes weitere
     *  Stueck beginnt also bei Versatz null.
     * ==================================================================== */

    if (m->Va != 0 &&
        p >= (UCHAR *)m->Va && p < (UCHAR *)m->Va + m->Len) {

        seg[0].ds_addr = m->Pa.LowPart + (ULONG)(p - (UCHAR *)m->Va);
        seg[0].ds_len  = len;

        if (cb != 0) {
            cb(arg, seg, 1, 0);
        }

        return 0;
    }

    while (rest != 0 && n < NTU_MAX_SEG) {

        ULONG bis_seitenende = PAGE_SIZE - (((ULONG_PTR)p) & PAGE_MASK);
        ULONG stueck = (rest < bis_seitenende) ? rest : bis_seitenende;

        pa = MmGetPhysicalAddress(p);

        if (pa.HighPart != 0) {

            /*  Ueber 4 GB.  bus_addr_t ist 32 Bit -- ein abgeschnittener
             *  Zeiger waere ein Baustein, der in fremden Speicher
             *  schreibt.  */

            if (cb != 0) {
                cb(arg, seg, 0, EINVAL);
            }

            return EINVAL;
        }

        /*  Anschliessende Seiten zusammenfassen: das haelt die Zahl der
         *  Stuecke klein und ist bei einem Puffer, den der Pool am
         *  Stueck vergeben hat, der Normalfall.  */

        if (n != 0 && seg[n - 1].ds_addr + seg[n - 1].ds_len == pa.LowPart) {
            seg[n - 1].ds_len += stueck;
        } else {
            seg[n].ds_addr = pa.LowPart;
            seg[n].ds_len  = stueck;
            n += 1;
        }

        p    += stueck;
        rest -= stueck;
    }

    if (rest != 0) {

        ntu_printf("usb: %u Byte brauchen mehr als %u Stuecke\n",
                   (ULONG)len, NTU_MAX_SEG);

        if (cb != 0) {
            cb(arg, seg, 0, EINVAL);
        }

        return EINVAL;
    }

    m->Va  = 0;                 /* kein gemeinsamer Puffer */
    m->Len = (ULONG)len;

    if (cb != 0) {
        cb(arg, seg, (int)n, 0);
    }

    return 0;
}

void
bus_dmamap_unload (bus_dma_tag_t t, bus_dmamap_t m)
{
    if (m != 0) {
        m->Va = 0;
        m->Len = 0;
    }
}

void
bus_dmamap_sync (bus_dma_tag_t t, bus_dmamap_t m, int op)
{
    /*  Leer, weil CacheEnable = FALSE.  Ausdruecklich leer und nicht
     *  fortgelassen -- siehe Kommentar oben.  */
    (void)t; (void)m; (void)op;
}


/* ====================================================================
 *  Was noch fehlt
 *
 *  Absichtlich hier aufgezaehlt und nicht verschwiegen.  Die Zahlen
 *  stammen aus der Messung, nicht aus der Schaetzung.
 *
 *      newbus-Baum        device_add_child und die Geschwisterliste.
 *                         Geradeaus, aber Fleissarbeit -- der USB-Stapel
 *                         haengt an jedem Hub Kinder ein.
 *      callout            KTIMER plus KDPC, der Faden aus diesem Modul
 *                         als Ziel.
 *      PCI                HalGetBusDataByOffset, ivars aus IopScanPciBus.
 *      bus_alloc_resource MmMapIoSpace fuer BAR0, Portnummer fuer BAR4.
 *      kproc_kthread_add  PsCreateSystemThread, zwei Faden je Bus.
 *
 *  Nach Schritt 3 des Skills ("nachmessen, die Zahl muss deutlich
 *  fallen") steht die Kante bei 127 Namen, von denen 96 in den
 *  Kopfdateien erledigt sind.  Die verbleibenden 31 sind die Liste oben.
 * ==================================================================== */


/* ====================================================================
 *  Ein Geraetename mit einer Nummer darin
 *
 *  NT 4.0 exportiert kein swprintf.  Der Name ist kurz und das Muster
 *  fest -- eine eigene Fassung ist kleiner als jede Abhaengigkeit, und
 *  sie kann nur eine einzige Umwandlung: %u.
 * ==================================================================== */

VOID
ntu_wsprintf (PWCHAR ziel, ULONG max, PCWSTR muster, ULONG zahl)
{
    ULONG i = 0;
    ULONG q = 0;

    while (muster[q] != 0 && i + 1 < max) {

        if (muster[q] == L'%' && muster[q + 1] == L'u') {

            WCHAR  d[12];
            ULONG  n = 0;
            ULONG  v = zahl;

            do {
                d[n] = (WCHAR)(L'0' + (v % 10));
                n += 1;
                v /= 10;
            } while (v != 0 && n < 12);

            while (n != 0 && i + 1 < max) {
                n -= 1;
                ziel[i] = d[n];
                i += 1;
            }

            q += 2;
            continue;
        }

        ziel[i] = muster[q];
        i += 1;
        q += 1;
    }

    ziel[i] = 0;
}


/* ====================================================================
 *  Reste
 *
 *  Vier Namen, die der Binderlauf gefunden hat.  Alle vier waren
 *  deklariert und nie hingeschrieben -- der Syntaxlauf sieht so etwas
 *  nicht, der Binder schon.  Genau dafuer ist die Prueflisten-Zeile
 *  "jeder Import gegen die echten Binaerdateien" im Skill.
 * ==================================================================== */

int  rebooting = 0;       /* NT meldet uns kein Herunterfahren */

struct mtx Giant;         /* nie genommen; nur damit der Verweis aufgeht */
struct mtx usb_ref_lock;  /* usb_hub.c nimmt sie beim Abziehen */

char *
ntu_strdup (const char *s, int flags)
{
    size_t n;
    char  *p;

    if (s == 0) {
        return 0;
    }

    n = strlen(s) + 1;
    p = (char *)ntu_malloc(n, 0);

    if (p != 0) {
        memcpy(p, s, n);
    }

    return p;
}

int
sysctl_handle_int (struct sysctl_oid *o, void *arg1, int arg2,
                   struct sysctl_req *req)
{
    return 0;             /* kein sysctl-Baum, siehe sys/sysctl.h */
}

/*  usbd_dummy_timeout: aeltere FreeBSD-Staende deklarieren ihn nur
 *  (usbdi.h), neuere definieren ihn in usb_core.c.  Damit beide binden,
 *  heisst die NT-Fassung anders und springt nur als Ausweichname ein --
 *  bringt usb_core.obj das Symbol mit, gilt dessen Fassung.  */
#pragma comment(linker, "/alternatename:_usbd_dummy_timeout@4=_ntu_dummy_timeout@4")

void
ntu_dummy_timeout (void *arg)
{
    /*  usbdi.h setzt diesen Zeiger als Anfangswert in jeden callout.
     *  Er darf nie laufen; laeuft er doch, ist ein callout gestartet
     *  worden, ohne dass jemand eine Routine eingetragen hat.  */

    ntu_printf("usb: usbd_dummy_timeout gerufen -- callout ohne "
               "Routine gestartet\n");
}
