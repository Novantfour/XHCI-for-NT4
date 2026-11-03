/*
 *  ober/ntkbdmou.h -- die NT-Seite der Tastatur- und Mausanbindung.
 *
 *  GEGEN DAS ECHTE DDK GEPRUEFT.  Quellen:
 *
 *      ddk/src/input/inc/NTDDKBD.H    KEYBOARD_INPUT_DATA, KEY_*,
 *                                     KEYBOARD_ATTRIBUTES, die
 *                                     METHOD_BUFFERED-IOCTLs
 *      ddk/src/input/inc/NTDDMOU.H    MOUSE_INPUT_DATA
 *      ddk/src/input/inc/KBDMOU.H     CONNECT_DATA, die
 *                                     METHOD_NEITHER-IOCTLs
 *
 *  Alle Lagen, alle Flagwerte und alle IOCTL-Nummern stimmen mit dem
 *  Nachbau ueberein -- die Zusicherungen unten sind damit nicht mehr
 *  Vermutung, sondern Absicherung gegen spaetere Aenderungen.
 *
 *  Eine Abweichung in der Schreibweise: das DDK legt Buttons als
 *  namenlose Vereinigung ueber ButtonFlags und ButtonData.  Hier steht
 *  sie flach, weil C89 namenlose Vereinigungen nicht kennt.  Die Lage
 *  ist dieselbe; die Zusicherungen belegen es.
 */

#ifndef _OBER_NTKBDMOU_H_
#define _OBER_NTKBDMOU_H_
#include <sys/param.h>
#include <sys/cdefs.h>

/*
 *  Mit -DNTU_ECHTES_DDK werden die Originalkopfdateien benutzt statt
 *  des Nachbaus.  Empfohlen, sobald mit clang-cl gegen das DDK gebaut
 *  wird: dann kann sich nichts mehr auseinanderentwickeln.  Der Nachbau
 *  bleibt fuer den Pruefbau, weil NTDDMOU.H eine namenlose Vereinigung
 *  benutzt, die C89 nicht kennt, und weil die Originale <ntddk.h> und
 *  darueber die MSVC-Kopfdateien ziehen.
 *
 *  Die Kopien liegen unter ober/ddk/ -- unveraendert, nur kleingeschrieben.
 */
#ifdef NTU_ECHTES_DDK
#include <ober/ddk/ntddkbd.h>
#include <ober/ddk/ntddmou.h>
#include <ober/ddk/kbdmou.h>
#define NTU_KBD_CONNECT       CONNECT_DATA
#define NTU_MOU_CONNECT       CONNECT_DATA
#define PNTU_SERVICE_CALLBACK PSERVICE_CALLBACK_ROUTINE
#else

#define FILE_DEVICE_KEYBOARD  0x0000000B
#define FILE_DEVICE_MOUSE     0x0000000F

#define NTU_CTL(dev, fn, meth)  (((dev) << 16) | ((fn) << 2) | (meth))

#define IOCTL_KEYBOARD_QUERY_ATTRIBUTES     NTU_CTL(FILE_DEVICE_KEYBOARD, 0x000, 0)
#define IOCTL_KEYBOARD_QUERY_INDICATORS     NTU_CTL(FILE_DEVICE_KEYBOARD, 0x010, 0)
#define IOCTL_KEYBOARD_SET_INDICATORS       NTU_CTL(FILE_DEVICE_KEYBOARD, 0x002, 0)
#define IOCTL_KEYBOARD_QUERY_TYPEMATIC      NTU_CTL(FILE_DEVICE_KEYBOARD, 0x008, 0)
#define IOCTL_KEYBOARD_SET_TYPEMATIC        NTU_CTL(FILE_DEVICE_KEYBOARD, 0x001, 0)
#define IOCTL_INTERNAL_KEYBOARD_CONNECT     NTU_CTL(FILE_DEVICE_KEYBOARD, 0x080, 3)
#define IOCTL_INTERNAL_KEYBOARD_DISCONNECT  NTU_CTL(FILE_DEVICE_KEYBOARD, 0x100, 3)
#define IOCTL_INTERNAL_KEYBOARD_ENABLE      NTU_CTL(FILE_DEVICE_KEYBOARD, 0x200, 3)
#define IOCTL_INTERNAL_KEYBOARD_DISABLE     NTU_CTL(FILE_DEVICE_KEYBOARD, 0x400, 3)

#define IOCTL_MOUSE_QUERY_ATTRIBUTES        NTU_CTL(FILE_DEVICE_MOUSE, 0x000, 0)
#define IOCTL_INTERNAL_MOUSE_CONNECT        NTU_CTL(FILE_DEVICE_MOUSE, 0x080, 3)
#define IOCTL_INTERNAL_MOUSE_DISCONNECT     NTU_CTL(FILE_DEVICE_MOUSE, 0x100, 3)
#define IOCTL_INTERNAL_MOUSE_ENABLE         NTU_CTL(FILE_DEVICE_MOUSE, 0x200, 3)
#define IOCTL_INTERNAL_MOUSE_DISABLE        NTU_CTL(FILE_DEVICE_MOUSE, 0x400, 3)

/*  Flags in KEYBOARD_INPUT_DATA.  KEY_E0 ist ein Flag, kein eigenes
 *  Ereignis -- das ist der Unterschied zum Drahtprotokoll des
 *  Tastaturbausteins und die haeufigste Verwechslung beim Nachbau.  */
#define KEY_MAKE   0x0000
#define KEY_BREAK  0x0001
#define KEY_E0     0x0002
#define KEY_E1     0x0004

typedef struct _KEYBOARD_INPUT_DATA {
    USHORT  UnitId;
    USHORT  MakeCode;
    USHORT  Flags;
    USHORT  Reserved;
    ULONG   ExtraInformation;
} KEYBOARD_INPUT_DATA, *PKEYBOARD_INPUT_DATA;

CTASSERT(sizeof(KEYBOARD_INPUT_DATA) == 12);
CTASSERT(__offsetof(KEYBOARD_INPUT_DATA, MakeCode) == 2);
CTASSERT(__offsetof(KEYBOARD_INPUT_DATA, Flags) == 4);
CTASSERT(__offsetof(KEYBOARD_INPUT_DATA, ExtraInformation) == 8);

#define MOUSE_LEFT_BUTTON_DOWN    0x0001
#define MOUSE_LEFT_BUTTON_UP      0x0002
#define MOUSE_RIGHT_BUTTON_DOWN   0x0004
#define MOUSE_RIGHT_BUTTON_UP     0x0008
#define MOUSE_MIDDLE_BUTTON_DOWN  0x0010
#define MOUSE_MIDDLE_BUTTON_UP    0x0020
#define MOUSE_WHEEL               0x0400
#define MOUSE_MOVE_RELATIVE       0x0000
#define MOUSE_MOVE_ABSOLUTE       0x0001

typedef struct _MOUSE_INPUT_DATA {
    USHORT  UnitId;
    USHORT  Flags;
    USHORT  ButtonFlags;
    USHORT  ButtonData;
    ULONG   RawButtons;
    LONG    LastX;
    LONG    LastY;
    ULONG   ExtraInformation;
} MOUSE_INPUT_DATA, *PMOUSE_INPUT_DATA;

CTASSERT(sizeof(MOUSE_INPUT_DATA) == 24);
CTASSERT(__offsetof(MOUSE_INPUT_DATA, ButtonFlags) == 4);
CTASSERT(__offsetof(MOUSE_INPUT_DATA, LastX) == 12);
CTASSERT(__offsetof(MOUSE_INPUT_DATA, LastY) == 16);

/*  Der Rueckruf, den kbdclass bzw. mouclass hinterlegt.  Er MUSS auf
 *  DISPATCH_LEVEL gerufen werden.  */
typedef VOID (*PNTU_SERVICE_CALLBACK)(PDEVICE_OBJECT DeviceObject,
                                      PVOID InputStart,
                                      PVOID InputEnd,
                                      PULONG InputConsumed);

typedef struct _NTU_KBD_CONNECT {
    PDEVICE_OBJECT        ClassDeviceObject;
    PNTU_SERVICE_CALLBACK ClassService;
} NTU_KBD_CONNECT, NTU_MOU_CONNECT;

CTASSERT(sizeof(NTU_KBD_CONNECT) == 8);

typedef struct _KEYBOARD_ID { UCHAR Type; UCHAR Subtype; } KEYBOARD_ID;

typedef struct _KEYBOARD_TYPEMATIC_PARAMETERS {
    USHORT UnitId;
    USHORT Rate;
    USHORT Delay;
} KEYBOARD_TYPEMATIC_PARAMETERS;

/*  Leuchten -- wie ntddkbd.h.  */
typedef struct _KEYBOARD_INDICATOR_PARAMETERS {
    USHORT UnitId;
    USHORT LedFlags;
} KEYBOARD_INDICATOR_PARAMETERS, *PKEYBOARD_INDICATOR_PARAMETERS;
#define KEYBOARD_SCROLL_LOCK_ON  1
#define KEYBOARD_NUM_LOCK_ON     2
#define KEYBOARD_CAPS_LOCK_ON    4

typedef struct _KEYBOARD_ATTRIBUTES {
    KEYBOARD_ID                   KeyboardIdentifier;
    USHORT                        KeyboardMode;
    USHORT                        NumberOfFunctionKeys;
    USHORT                        NumberOfIndicators;
    USHORT                        NumberOfKeysTotal;
    ULONG                         InputDataQueueLength;
    KEYBOARD_TYPEMATIC_PARAMETERS KeyRepeatMinimum;
    KEYBOARD_TYPEMATIC_PARAMETERS KeyRepeatMaximum;
} KEYBOARD_ATTRIBUTES;

typedef struct _MOUSE_ATTRIBUTES {
    USHORT MouseIdentifier;
    USHORT NumberOfButtons;
    USHORT SampleRate;
    ULONG  InputDataQueueLength;
} MOUSE_ATTRIBUTES;

#endif /* NTU_ECHTES_DDK */

struct ntukbd_softc;
struct ntums_softc;

/*  Die beiden Anschluesse entstehen EINMAL, in DriverEntry, und leben bis
 *  zum Entladen.  Eine USB-Tastatur, die spaeter aufgezaehlt wird, meldet
 *  sich nur an und ab -- sie legt nichts an.
 *
 *  Grund: kbdclass zaehlt HARDWARE\DEVICEMAP\KeyboardPort in seinem
 *  DriverEntry, einmal, beim Booten.  Was danach entsteht, oeffnet es nie.
 *  Die erste Fassung legte den Anschluss beim Aufzaehlen an; im
 *  QEMU-Lauf stand dann "Anschluss KeyboardPort1 angelegt" im Bericht,
 *  und die Tastatur war trotzdem tot.  */

extern KEYBOARD_TYPEMATIC_PARAMETERS ntu_typematik;

NTU_KBD_CONNECT *ntu_kbd_anmelden(void);
NTU_MOU_CONNECT *ntu_mou_anmelden(void);
void             ntu_kbd_abmelden(void);
void             ntu_kbd_leuchten(USHORT nt);      /* IOCTL_KEYBOARD_SET_INDICATORS */
USHORT           ntu_kbd_leuchten_jetzt(void);     /* ... QUERY_INDICATORS          */
void             ntu_mou_abmelden(void);
void             ntu_port_ende(void);

#define NTU_PORT_KBD     0
#define NTU_PORT_MAUS    1
#define NTU_PORT_STEUER  2
#define NTU_PORT_UGEN    3
#define NTU_PORT_PLATTE  4      /* ober/ntumass.c: USB-Massenspeicher */
#define NTU_PORT_JOY     5      /* ober/ntujoy.c: Gamepads, Joysticks */
#define NTU_PORT_DRUCK   6      /* ober/ntuprn.c: USB-Drucker */

typedef struct ntu_port {
    ULONG             Kind;
    ULONG             Suffix;
    void             *Softc;
    NTU_KBD_CONNECT  *Conn;
} ntu_port;

VOID ntu_port_init(PDRIVER_OBJECT, PUNICODE_STRING);

struct _IRP;
NTSTATUS ntu_steuer_ioctl(struct _IRP *, ULONG code, PVOID puffer,
                          ULONG ein, ULONG aus, PULONG len);
int  ntu_steuer_create(VOID);
NTSTATUS ntu_ugen_ioctl(PDEVICE_OBJECT, ULONG code, PVOID puffer,
                        ULONG ein, ULONG aus, PULONG len);
/*  Maustasten 1..8 (ober/ntmaustasten.c).  Jede hat eine Belegung;
 *  Vorgabe 1 = klick-links, 2 = klick-rechts, 3 = mitte, 4 = alt+links,
 *  5 = alt+rechts, 6..8 = aus.  */
#define NTU_MAUS_TASTEN  8
#define NTU_MAUS_EXTRA   NTU_MAUS_TASTEN
enum { NTU_MA_NICHTS, NTU_MA_TASTEN, NTU_MA_MITTE, NTU_MA_DOPPEL,
       NTU_MA_LINKS, NTU_MA_RECHTS };
typedef struct {
    int    art;
    int    n;
    USHORT code[6];          /* Satz-1-Codes, 0x100 = E0-Vorsatz */
    char   text[48];         /* die Angabe, wie gesetzt (klein) */
} NTU_MAUSAKTION;
extern NTU_MAUSAKTION ntu_mausaktion[NTU_MAUS_TASTEN];
extern KSPIN_LOCK     ntu_maus_sperre;     /* schuetzt ntu_mausaktion */
extern volatile LONG  ntu_maus_roh;        /* Tasten der zuletzt meldenden Maus, Bit 0 = 1 */
extern volatile LONG  ntu_maus_berichte;   /* Berichte seit dem Start */
extern volatile LONG  ntu_maus_knoepfe;    /* groesste Tastenzahl einer USB-Maus */
extern volatile LONG  ntu_maus_anzahl;     /* angesteckte USB-Maeuse */
extern volatile LONG  ntu_maus_leer;       /* leere Berichte, nicht weitergegeben */
NTSTATUS ntu_maustaste_setzen(int taste, const char *text);   /* 1..8, sofort + Registry */
void     ntu_maustaste_holen(int taste, char *ziel, int max);
struct _NTU_MAUS_PROTOKOLL;
void     ntu_maus_protokoll_holen(struct _NTU_MAUS_PROTOKOLL *p);
VOID ntu_maustasten_lesen(PUNICODE_STRING reg);
int  ntu_belegung_lesen(const char *s, NTU_MAUSAKTION *a);   /* 0 = verstanden */

/*  Sondertasten (ntsondertasten.c, Tabelle ntsonder.h).  */
#include <ober/ntsonder.h>
VOID     ntu_sondertasten_lesen(PUNICODE_STRING reg);
int      ntu_sonder_index(unsigned seite, unsigned nutzung);   /* -1: keine */
VOID     ntu_sonder_taste(int index, int gedrueckt);
void     ntu_sonder_zustand(NTU_SONDER_ZUSTAND *z);
void     ntu_sonder_holen(int index, char *ziel, int max);
NTSTATUS ntu_sonder_setzen(int index, const char *text);
int      ntu_sonder_abholen(NTU_SONDER_AUFTRAG *a);
extern volatile LONG ntu_sonder_tastaturen;

/*  Tasten an kbdclass geben, ohne USB-Tastatur (ntkbdport.c).
 *  los = 0: druecken in Reihenfolge; los = 1: loslassen umgekehrt.  */
VOID ntu_kbd_einspeisen(const USHORT *codes, int n, int los);

NTSTATUS ntum_irp(PDEVICE_OBJECT, PIRP);      /* Lesen, Schreiben, Leeren */
NTSTATUS ntum_ioctl(PDEVICE_OBJECT, PIRP);    /* IOCTL_DISK_*            */
NTSTATUS ntuj_ioctl(PDEVICE_OBJECT, PIRP);    /* IOCTL_NTUJ_*            */
NTSTATUS ntup_irp(PDEVICE_OBJECT, PIRP);      /* Schreiben, Aufraeumen   */
NTSTATUS ntup_ioctl(PDEVICE_OBJECT, PIRP);    /* IOCTL_NTUP_*            */
VOID     ntup_parameter_lesen(PUNICODE_STRING); /* SchreibWartezeit   */
extern ULONG ntu_xhci_schritt;                  /* Parameters\XhciSchritt */
void ntu_xhci_sonder(device_t, unsigned vid, unsigned did);   /* ober/ntxhcisonder.c */
VOID ntu_steuer_delete(VOID);


extern PDRIVER_OBJECT NtuDriverObject;
#endif
