/*
 *  nt/ntunterlage.h -- die NT-Seite der Unterlage.
 *
 *  Jede sys/*.h der Unterlage zieht diese Datei; sie ist der einzige Ort,
 *  an dem NT-Namen vorkommen.  Die Kopfdateien darueber bilden nur ab.
 *
 *  Zwei Entwurfsentscheidungen stehen hier und muessen vor der ersten
 *  Zeile Treiberarbeit verstanden sein:
 *
 *  1.  Die Unterbrechung laeuft in einem Faden, nicht im ISR.
 *
 *      FreeBSDs USB-Stapel nimmt in seinem Unterbrechungspfad Mutexe und
 *      wartet auf Bedingungsvariablen.  Unter FreeBSD ist das erlaubt,
 *      weil die Unterbrechung dort ein ithread im Prozesskontext ist.
 *      Unter NT ist es ein Absturz: auf DIRQL darf kein KMUTEX genommen
 *      werden, und ein Seitenfehler dort ist ein Bugcheck.
 *
 *      Nachgebildet wird deshalb genau der FreeBSD-Aufbau:
 *
 *          NT-ISR      quittiert nur und sperrt die Unterbrechung
 *                      am Baustein selbst -- sonst haelt die
 *                      pegelgesteuerte PCI-Leitung an und der Rechner
 *                      steht in einem Unterbrechungssturm
 *          DPC         setzt ein KEVENT
 *          Fadenkern   ruft den echten FreeBSD-Handler auf
 *                      PASSIVE_LEVEL und gibt die Unterbrechung
 *                      am Baustein wieder frei
 *
 *      Damit ist mtx -> KMUTEX, cv -> KEVENT, sx -> KMUTEX durchweg
 *      zulaessig, und der Treiber bleibt Zeile fuer Zeile unveraendert.
 *      Der Preis ist Latenz: zwei Fadenwechsel je Unterbrechung.  Fuer
 *      USB 1.1 und 2.0 traegt das.
 *
 *  2.  Keine 64-Bit-Arithmetik.
 *
 *      Gebunden wird mit /nodefaultlib; __allmul und __aulldiv gibt es
 *      nicht.  Die Unterlage stellt deshalb keine Hilfsmittel bereit,
 *      die dazu verleiten.  htole64 und le64toh sind reine Byteschieber
 *      und daher erlaubt.
 */

/*  Dateifassung, wie in der Versionsressource -- setzt bau.sh.  */
#ifndef NTU_DATEIFASSUNG
#define NTU_DATEIFASSUNG "unbekannt"
#endif


#ifndef _NT_UNTERLAGE_H_
#define _NT_UNTERLAGE_H_

#include <ntddk.h>
#include <stdarg.h>
#ifdef NTU_FREIE_KOEPFE
#include "ntfreiekoepfe.h"
#endif

/*  ULONG_PTR und LONG_PTR gibt es im NT-4.0-DDK noch nicht -- sie kamen
 *  erst mit der Vorbereitung auf 64 Bit.  Auf i386 sind sie genau so
 *  breit wie ULONG; hier stehen sie, damit der Quelltext dieselben
 *  Namen benutzen kann wie ein heutiger.  */

#ifndef _ULONG_PTR_DEFINED_
#define _ULONG_PTR_DEFINED_
typedef unsigned long ULONG_PTR, *PULONG_PTR;
typedef long          LONG_PTR,  *PLONG_PTR;
typedef unsigned long UINT_PTR;
typedef long          INT_PTR;
#endif

/* ------------------------------------------------------------------ *
 *  Merkbrocken
 * ------------------------------------------------------------------ */

#define NTU_TAG  'BSUn'

/*  Stuecke je Abbildung.  Ein Puffer aus dem Pool liegt fast immer am
 *  Stueck; 64 Seiten sind 256 kB und damit mehr, als der USB-Stapel je
 *  in einem Zug anfordert.  */
#define NTU_MAX_SEG  64          /* "nUSB" rueckwaerts im Speicherabzug */

/* ------------------------------------------------------------------ *
 *  Sperren
 *
 *  FreeBSD kennt drei Sorten; NT hat fuer jede eine Entsprechung, aber
 *  nicht dieselbe.  Die Zuordnung ist keine Geschmacksfrage:
 *
 *      mtx (MTX_DEF)   schlaeft, wird mit cv_wait abgegeben  -> KMUTEX
 *      mtx (MTX_SPIN)  sperrt gegen Unterbrechungen          -> KSPIN_LOCK
 *      sx              Leser/Schreiber, schlaeft             -> KMUTEX
 *
 *  Der USB-Stapel benutzt fast ausschliesslich MTX_DEF.  MTX_SPIN kommt
 *  nur an Stellen vor, die wir ohnehin nicht uebersetzen.
 * ------------------------------------------------------------------ */

typedef struct ntu_mtx {
    KMUTEX          Mutex;
    PKTHREAD        Owner;       /* fuer mtx_owned und KASSERT */
    ULONG           Depth;       /* MTX_RECURSE */
    const char     *Name;
    ULONG           Spin;        /* bleibt 0 -- siehe ntu_mtx_init */
} ntu_mtx;

typedef struct ntu_cv {
    KEVENT          Event;       /* SynchronizationEvent */
    const char     *Name;
    ULONG           Waiters;
} ntu_cv;

/*  Wer wartet worauf -- fuer den Herzschlag.
 *
 *  Jede Wartestelle der Unterlage (mtx_lock, cv_wait, cv_timedwait und
 *  die Bruecke in ntugen) traegt den wartenden Faden hier ein, mit Art,
 *  Name des Objekts, Zeitpunkt und -- bei einer Sperre -- dem Halter.
 *  Haengt ein Faden laenger, meldet der Herzschlag ihn samt Halter; ein
 *  Kreis ist dann an zwei Zeilen abzulesen.
 *
 *  Gebaut, als nach dem Abziehen eines Geraets der Aufzaehlungsfaden
 *  eines Busses stehenblieb und von aussen nicht zu sehen war, worauf.  */

#define NTU_WART_MAX  48

typedef struct ntu_wart {
    PKTHREAD     Faden;
    const char  *FadenName;
    const char  *Art;          /* 0 = wartet nicht */
    const char  *Worauf;
    PKTHREAD     Halter;       /* nur bei Sperren */
    int          Seit;         /* ticks */
} ntu_wart;

extern ntu_wart ntu_wart_tab[NTU_WART_MAX];

VOID  ntu_wart_name  (const char *);
VOID  ntu_wart_rein  (const char *art, const char *worauf, PKTHREAD halter);
VOID  ntu_wart_raus  (VOID);

VOID  ntu_mtx_init   (ntu_mtx *, const char *, const char *, int);
VOID  ntu_mtx_destroy(ntu_mtx *);
VOID  ntu_mtx_lock   (ntu_mtx *);
VOID  ntu_sysctl_lesen (PUNICODE_STRING);   /* nt/ntsysctl.c */
VOID  ntu_mtx_unlock (ntu_mtx *);
int   ntu_mtx_owned  (ntu_mtx *);

VOID  ntu_cv_init    (ntu_cv *, const char *);
VOID  ntu_cv_destroy (ntu_cv *);
VOID  ntu_cv_wait    (ntu_cv *, ntu_mtx *);
int   ntu_cv_timedwait(ntu_cv *, ntu_mtx *, int timo);
VOID  ntu_cv_signal  (ntu_cv *);
VOID  ntu_cv_broadcast(ntu_cv *);

/* ------------------------------------------------------------------ *
 *  Registerzugriff
 *
 *  bus_space traegt in FreeBSD zwei Dinge: ein Kennzeichen fuer den
 *  Adressraum (tag) und die Basis darin (handle).  Beides passt in NT
 *  ohne Verlust -- der tag sagt nur, ob Speicher oder E/A.
 * ------------------------------------------------------------------ */

#define NTU_SPACE_MEM  0
#define NTU_SPACE_IO   1

typedef ULONG_PTR  ntu_space_tag;     /* NTU_SPACE_MEM oder NTU_SPACE_IO */
typedef PUCHAR     ntu_space_handle;  /* gemappte Basis bzw. Portnummer  */

/* ------------------------------------------------------------------ *
 *  DMA
 *
 *  FreeBSDs bus_dma kennt tag (Einschraenkungen) und map (eine konkrete
 *  Abbildung).  Der USB-Stapel benutzt davon nur den einfachen Fall:
 *  ein zusammenhaengender, gleichzeitig von CPU und Baustein sichtbarer
 *  Puffer.  Das ist HalAllocateCommonBuffer, eins zu eins.
 *
 *  Streuung/Sammlung braucht der Stapel nicht -- er kopiert in seine
 *  eigenen Puffer.  bus_dmamap_load bleibt deshalb auf den Einzelfall
 *  beschraenkt und meldet EINVAL, wenn mehr verlangt wird.  Lieber ein
 *  ehrlicher Fehler als eine halbe Streuliste.
 * ------------------------------------------------------------------ */

typedef struct ntu_dma_tag {
    PADAPTER_OBJECT Adapter;
    ULONG           Alignment;
    ULONG           MaxSize;
    ULONG           NSegments;
    PDEVICE_OBJECT  Device;
} *ntu_dma_tag_t;

typedef struct ntu_dma_map {
    ntu_dma_tag_t   Tag;
    PVOID           Va;          /* ausgerichtet, fuer den Treiber   */
    PHYSICAL_ADDRESS Pa;
    ULONG           Len;
    PVOID           RohVa;       /* wie angefordert, fuer das
                                    Freigeben -- HalFreeCommonBuffer
                                    will genau diese Werte zurueck   */
    PHYSICAL_ADDRESS RohPa;
    ULONG           RohLen;
    BOOLEAN         Cached;
} *ntu_dma_map_t;

/* ------------------------------------------------------------------ *
 *  Geraet (newbus)
 *
 *  FreeBSDs device_t ist ein Knoten in einem Baum mit Eltern, Kindern,
 *  softc und ivars.  Der USB-Stapel benutzt den Baum wirklich: jeder
 *  Hub haengt Kinder ein.  Er wird deshalb nachgebaut, nicht auf
 *  NT-DEVICE_OBJECTs abgebildet -- nur der Wurzelknoten haelt ein
 *  echtes DEVICE_OBJECT.
 * ------------------------------------------------------------------ */

typedef struct ntu_device {
    struct ntu_device  *Parent;
    LIST_ENTRY          Siblings;
    LIST_ENTRY          Children;
    PVOID               Softc;
    ULONG               SoftcSize;
    BOOLEAN             SoftcOwned;
    PVOID               Ivars;
    CHAR                Name[32];
    CHAR                Desc[64];
    CHAR                Nameunit[40];
    ULONG               Unit;
    BOOLEAN             Attached;
    BOOLEAN             Quiet;
    struct driver      *Driver;   /* Lage in sys/bus.h, FreeBSD-konform */
    PDEVICE_OBJECT      Pdo;      /* nur Wurzel */
    struct resource    *Res;      /* der Registerblock, fuer den ISR */
    struct ntu_dma_tag *DmaTag;   /* je Hostbaustein einer */
    void               *Intr;     /* ntu_intr, fuer den Herzschlag */
} *ntu_device_t;

/*  driver_t liegt in sys/bus.h -- es muss die FreeBSD-Lage haben, weil
 *  die Treiber es mit benannten Feldern fuellen.  */
struct driver;

int  ntu_attach_softc(ntu_device_t);
void ntu_detach_softc(ntu_device_t);
ntu_device_t ntu_root_create (PDEVICE_OBJECT);
struct ntu_pci_ivars;
ntu_device_t ntu_root_add_pci(struct ntu_pci_ivars *);

/* ------------------------------------------------------------------ *
 *  Faden statt ithread
 * ------------------------------------------------------------------ */

struct callout;
struct resource;
VOID  ntu_work_start(VOID);
VOID  ntu_work_stop (VOID);
ULONG ntu_work_verloren(VOID);
int   ntu_defer_callout(struct callout *);

typedef struct ntu_thread {
    HANDLE      Handle;
    PKTHREAD    Thread;
    KEVENT      Wake;
    KEVENT      Gone;
    VOID      (*Body)(PVOID);
    PVOID       Arg;
    BOOLEAN     Stop;
    const char     *Name;
} ntu_thread;

NTSTATUS ntu_thread_start(ntu_thread *, VOID (*)(PVOID), PVOID,
                          const char *);
VOID     ntu_thread_stop (ntu_thread *);

/* ------------------------------------------------------------------ *
 *  Ausgabe
 *
 *  KiSerialPutString ist der 1017. Export des selbstgebauten Kernels
 *  und schreibt ohne Debugger auf COM1.  Genau dafuer ist er da -- und
 *  genau deshalb steht er NICHT in der NTOSKRNL.LIB des DDK.
 *
 *  Der Binder braucht dafuer eine eigene Einfuhrbibliothek; sie wird
 *  aus pruef/kiserial.def erzeugt (siehe bau.sh).  Wer den Treiber auf
 *  einem unveraenderten NT 4.0 SP6a laden will, baut mit
 *
 *      -DNTU_OHNE_KISERIAL
 *
 *  und bekommt DbgPrint statt der seriellen Ausgabe.  Ohne angehaengten
 *  Kerndebugger geht die dann ins Leere -- das ist der Preis, und er
 *  gehoert gesagt statt verschwiegen.
 * ------------------------------------------------------------------ */

/*  Drei Wege, und die Wahl faellt beim Bauen:
 *
 *      Vorgabe              eigener UART-Schreiber, COM1 = 0x3F8.
 *                           Laeuft auf JEDEM NT-4.0-Kern, auch dem
 *                           unveraenderten von Microsoft.
 *      -DNTU_KISERIAL       KiSerialPutString, der 1017. Export des
 *                           selbstgebauten Kerns.  Teilt sich den
 *                           Anschluss sauber mit dessen Bericht.
 *      -DNTU_OHNE_SERIELL   DbgPrint.  Ohne angehaengten Kerndebugger
 *                           geht die Ausgabe ins Leere.
 *
 *  Der eigene Schreiber ist die Vorgabe geworden, weil der erste
 *  Versuch auf einem Standardkern gar nicht erst geladen haette: der
 *  Lader findet KiSerialPutString dort nicht, und der Treiber bleibt
 *  stumm, bevor er irgendetwas sagen kann.  */

#if defined(NTU_KISERIAL)
VOID  KiSerialPutString(PCHAR);
#define ntu_seriell_aus(s)  KiSerialPutString(s)
#elif defined(NTU_OHNE_SERIELL)
#define ntu_seriell_aus(s)  DbgPrint("%s", (s))
#else
VOID  ntu_seriell_aus(PCHAR);
VOID  ntu_ring_starten(VOID);     /* Ausgabe ueber Ringpuffer und Faden */
VOID  ntu_ring_anhalten(VOID);
#endif
#define KiSerialPutString(s)  ntu_seriell_aus(s)
VOID  ntu_printf(const char *, ...);
VOID  ntu_wsprintf(PWCHAR, ULONG, PCWSTR, ULONG);
ULONG ntu_pool_offen(VOID);
ULONG ntu_pool_vergaben(VOID);
ULONG ntu_pool_gemeckert(VOID);
struct sysctl_oid; struct sysctl_req;
int   sysctl_handle_int(struct sysctl_oid *, void *, int, struct sysctl_req *);
VOID  ntu_panic (const char *, ...);

/* Zeit.  hz ist in FreeBSD die Tickrate; NT rechnet in 100 ns. */

#define NTU_HZ          100
#define NTU_TICK_100NS  100000       /* 10 ms, NT-Takt */

VOID  ntu_delay_us(ULONG us);        /* KeStallExecutionProcessor, gestueckelt */
VOID  ntu_clock_start(VOID);
VOID  ntu_clock_stop (VOID);

/* ------------------------------------------------------------------ *
 *  Unterbrechung im Faden -- die Bauteile liegen hier, damit
 *  ntgeraet.c sie einhaengen kann.
 *
 *  Der Maskierer ist nicht Sache des Treibers: welches Register die
 *  Quelle abstellt, ist Wissen ueber den Baustein, und das gehoert in
 *  die Unterlage.  Ausgewaehlt wird ueber die Schnittstellenkennung.
 * ------------------------------------------------------------------ */

typedef struct ntu_intr {
    PKINTERRUPT        Object;
    KDPC               Dpc;
    KEVENT             Wake;
    struct ntu_thread *Thread;
    void            (*Handler)(void *);
    int             (*Filter) (void *);
    void              *Arg;

    /*  Alles, was der ISR zum Abstellen braucht.  Im ISR darf nichts
     *  nachgeschlagen werden.  */
    ntu_space_tag      Tag;
    ntu_space_handle   Handle;
    ULONG              OpBase;      /* EHCI/xHCI: cap + CAPLENGTH */
    ULONG              Iman;        /* nur xHCI: Unterbrecher 0      */
    UCHAR              ProgIf;
    ULONG              Saved;       /* USBINTR waehrend der Sperre */
    volatile LONG      Gesperrt;    /* 1 = Quelle steht ab, Faden arbeitet */
    ULONG              Unt;         /* je Baustein: angenommen    */
    ULONG              Fremd;       /* je Baustein: abgewiesen    */
    ULONG              FremdGesp;   /* davon, weil gesperrt       */
    ULONG              Nachsperre;  /* gesperrt, aber vom Handler
                                       wieder geoeffnet: nachgesperrt */
    volatile LONG      Stop;

    /*  Abfragebetrieb: keine nutzbare Leitung (BIOS liess 0/255 stehen,
     *  oder die Leitung passt nicht zur HAL, etwa APIC-Nummer 31 unter
     *  der PIC-HAL).  Dann weckt ein Zeitgeber den Faden; der FreeBSD-
     *  Handler prueft den Baustein ohnehin selbst.  */
    ULONG              Abfrage;
    KTIMER             Takt;
    KDPC               TaktDpc;
} ntu_intr;

/*  Die letzten Unterbrechungen, so wie der ISR sie vorgefunden hat.
 *  Zwei Werte je Eintrag genuegen, um die Frage zu beantworten, die
 *  aus den Zaehlern allein nicht hervorgeht: meldet der Baustein
 *  ueberhaupt ein Ereignis (USBSTS.EINT), und raeumt der Treiber es weg
 *  (IMAN.IP)?  Wiederholt sich dieselbe Kombination endlos, wird der
 *  Ereignisring nicht weitergeschoben.  */

#define NTU_SPUR_TIEFE 16

typedef struct ntu_spur {
    ULONG Iman;
    ULONG Usbsts;
    ULONG Tick;
} ntu_spur;

extern ntu_spur ntu_spur_ring[NTU_SPUR_TIEFE];
extern ULONG    ntu_spur_kopf;

BOOLEAN ntu_isr(PKINTERRUPT, PVOID);

/*  nt/ntamdpll.c -- AMD-PLL-Abhilfe fuer isochron, siehe dort.  */
VOID  ntu_amdpll_erkennen(VOID);
int   ntu_amdpll_betrifft(void *hc);        /* device_t */
VOID  ntu_amdpll_isoc_an(VOID);
VOID  ntu_amdpll_isoc_aus(VOID);
extern ULONG ntu_amdpll_aus;

/*  ober/ntxhcipcd.c -- siehe dort.  */
VOID  ntu_xhci_pcd_nachholen(void *softc, ntu_space_tag, ntu_space_handle,
                             ULONG opbase);
extern ULONG ntu_pcd_nachgeholt;
VOID    ntu_dpc(PKDPC, PVOID, PVOID, PVOID);
VOID    ntu_ithread_body(PVOID);
VOID    ntu_intr_mask  (ntu_intr *);
VOID    ntu_intr_unmask(ntu_intr *);
VOID  ntu_pause_ticks(ULONG n);  /* KeDelayExecutionThread */

#endif /* _NT_UNTERLAGE_H_ */
