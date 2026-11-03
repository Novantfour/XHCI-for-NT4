/*
 *  ober/ntkbdport.c -- die NT-Anschlussgeraete fuer Tastatur und Maus.
 *
 *  Wir sind Anschlusstreiber, nicht Filter.  kbdclass und mouclass
 *  suchen ein Geraeteobjekt, melden sich mit einem IOCTL an und
 *  hinterlegen dabei ihren Rueckruf.  Von da an liefern wir Daten.
 *
 *      \Device\KeyboardPort1     i8042prt hat 0
 *      \Device\PointerPort1
 *
 *  Die eigentliche Arbeit ist klein.  Was Muehe macht, sind die
 *  Abfrage-IOCTLs: kbdclass fragt vor dem Anmelden nach Eigenschaften
 *  und legt danach seine Warteschlange an.  Antwortet man nicht, haengt
 *  die Anmeldung, und zwar ohne Fehlermeldung.
 *
 *  WIE KBDCLASS UNS FINDET -- im DDK-Quelltext nachgelesen
 *  (ddk/src/input/kbdclass/KBDCLASS.C, KbdDeterminePortsServiced und
 *  KbdDeviceMapQueryCallback), nicht vermutet.  Das Verfahren ist ein
 *  anderes, als man erwartet, und es hat zwei harte Folgen:
 *
 *  1.  kbdclass sucht **keine** Geraetenamen ab.  Es zaehlt die Werte
 *      unter
 *
 *          HKLM\HARDWARE\DEVICEMAP\KeyboardPort
 *
 *      Der Rueckruf tut nichts weiter als *(PULONG)Context += 1 -- der
 *      Name des Wertes wird nicht einmal angesehen.  Wer sich dort
 *      nicht eintraegt, wird nie geoeffnet, ganz gleich wie das Geraet
 *      heisst.  i8042prt traegt sich mit RtlWriteRegistryValue ein
 *      (I8042DEP.C, Zeile 1223); wir tun dasselbe.
 *
 *  2.  Die Nummern muessen ab 0 luckenlos sein.  Der DDK-Quelltext sagt
 *      es woertlich: "The port device objects are created with suffixes
 *      in strictly ascending order, starting with suffix 0. There are
 *      no gaps in the list."  kbdclass baut die Namen aus der **Anzahl**
 *      zusammen, nicht aus den eingetragenen Namen.
 *
 *      Daraus folgt: wir nehmen die kleinste freie Nummer, auch die 0.
 *      Ein erster Entwurf begann bei 1, um i8042prt auszuweichen -- das
 *      ist genau in dem Fall falsch, auf den es ankommt: ohne
 *      PS/2-Tastatur traegt i8042prt nichts ein, kbdclass zaehlt 1 und
 *      oeffnet KeyboardPort0, den es dann nicht gibt.
 *
 *  Dazu die Einstellung, die man trotzdem braucht (KBDCLASS.C 2200 ff.,
 *  2314 ff.): MaximumPortsServiced hat den Vorgabewert 1 und wird auf
 *  die Anzahl im DEVICEMAP heruntergeklemmt.  ConnectMultiplePorts hat
 *  den Vorgabewert 0, woraus ConnectOneClassToOnePort = 1 folgt -- je
 *  Anschluss ein eigenes Klassengeraet.  Und im selben Quelltext steht:
 *  "the Raw Input Thread (Windows USER) will only deign to open and
 *  read from one keyboard device."
 *
 *  Steckt also zusaetzlich eine PS/2-Tastatur, landet die USB-Tastatur
 *  auf KeyboardClass1, und USER liest sie nie.  Gebraucht wird:
 *
 *      HKLM\SYSTEM\CurrentControlSet\Services\Kbdclass\Parameters
 *          MaximumPortsServiced  = 2    (REG_DWORD)
 *          ConnectMultiplePorts  = 1    (REG_DWORD)
 *
 *      ...\Services\Mouclass\Parameters   dasselbe
 *
 *  mouclass arbeitet identisch, nur mit "PointerClass"/"PointerPort"
 *  (MOUCLASS.C 400, 2207).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <ober/ntkbdmou.h>

PDRIVER_OBJECT NtuDriverObject;

/*  Der Registrierungspfad unseres eigenen Dienstes.  Er ist der Inhalt
 *  des DEVICEMAP-Wertes -- genau wie bei i8042prt, wo registryPath
 *  geschrieben wird.  */
static WCHAR ntu_regpfad[200];
static USHORT ntu_regpfad_len;


/* ====================================================================
 *  IOCTL
 * ==================================================================== */

static NTSTATUS
ntu_port_ioctl (PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sl  = IoGetCurrentIrpStackLocation(irp);
    ntu_port          *p   = (ntu_port *)dev->DeviceExtension;
    ULONG              code = sl->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS           st  = STATUS_SUCCESS;
    ULONG              len = 0;

    /*  Alle Geraete dieses Treibers teilen sich die Verteilung.  Das
     *  Steuergeraet fuer das Werkzeug hat nichts mit Tastatur oder Maus
     *  zu tun und geht sofort woandershin.  */

    if (p->Kind == NTU_PORT_PLATTE) {
        return ntum_ioctl(dev, irp);
    }

    /*  NTU_PORT_JOY gibt es hier nicht mehr: Gamepads sitzen in
     *  USBXBOX.SYS (eigener Treiber, eigene IRP-Verteilung).  */

    if (p->Kind == NTU_PORT_DRUCK) {
        return ntup_ioctl(dev, irp);
    }

    if (p->Kind == NTU_PORT_UGEN) {

        /*  Ein allgemein bereitgestelltes USB-Geraet -- ober/ntugen.c.  */

        st = ntu_ugen_ioctl(dev, code,
                            irp->AssociatedIrp.SystemBuffer,
                            sl->Parameters.DeviceIoControl.InputBufferLength,
                            sl->Parameters.DeviceIoControl.OutputBufferLength,
                            &len);

        irp->IoStatus.Status      = st;
        irp->IoStatus.Information = len;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }

    if (p->Kind == NTU_PORT_STEUER) {

        st = ntu_steuer_ioctl(irp, code,
                              irp->AssociatedIrp.SystemBuffer,
                              sl->Parameters.DeviceIoControl.InputBufferLength,
                              sl->Parameters.DeviceIoControl.OutputBufferLength,
                              &len);

        irp->IoStatus.Status      = st;
        irp->IoStatus.Information = len;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }

    switch (code) {

        case IOCTL_INTERNAL_KEYBOARD_CONNECT:
        case IOCTL_INTERNAL_MOUSE_CONNECT: {

            /*  METHOD_NEITHER: der Puffer steht in Type3InputBuffer,
             *  nicht in AssociatedIrp.SystemBuffer.  Wer hier
             *  SystemBuffer liest, bekommt Muell und traegt einen
             *  Rueckruf ins Nichts ein -- und merkt es erst beim ersten
             *  Tastendruck.  */

            NTU_KBD_CONNECT *c =
                (NTU_KBD_CONNECT *)sl->Parameters.DeviceIoControl
                                     .Type3InputBuffer;

            if (sl->Parameters.DeviceIoControl.InputBufferLength
                    < sizeof(NTU_KBD_CONNECT) || c == 0) {
                st = STATUS_INVALID_PARAMETER;
                break;
            }

            if (p->Conn->ClassService != 0) {
                st = STATUS_SHARING_VIOLATION;   /* schon angemeldet */
                break;
            }

            p->Conn->ClassDeviceObject = c->ClassDeviceObject;
            p->Conn->ClassService      = c->ClassService;

            ntu_printf("usb: %s angemeldet\n",
                       (p->Kind == 0) ? "kbdclass" : "mouclass");
            break;
        }

        case IOCTL_INTERNAL_KEYBOARD_DISCONNECT:
        case IOCTL_INTERNAL_MOUSE_DISCONNECT:
            p->Conn->ClassService      = 0;
            p->Conn->ClassDeviceObject = 0;
            break;

        case IOCTL_KEYBOARD_QUERY_ATTRIBUTES: {

            KEYBOARD_ATTRIBUTES *a =
                (KEYBOARD_ATTRIBUTES *)irp->AssociatedIrp.SystemBuffer;

            if (sl->Parameters.DeviceIoControl.OutputBufferLength
                    < sizeof(*a)) {
                st = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            memset(a, 0, sizeof(*a));

            /*  Typ 4, Untertyp 0 ist die uebliche 101/102-Tastatur.  NT
             *  sucht danach die Belegungsdatei aus; ein falscher Typ
             *  liefert eine Tastatur, auf der die Umschalttaste
             *  woanders sitzt.  */

            a->KeyboardIdentifier.Type    = 4;
            a->KeyboardIdentifier.Subtype = 0;
            a->KeyboardMode               = 1;      /* Abtastsatz 1 */
            a->NumberOfFunctionKeys       = 12;
            a->NumberOfIndicators         = 3;
            a->NumberOfKeysTotal          = 101;
            a->InputDataQueueLength       = 32 * sizeof(KEYBOARD_INPUT_DATA);
            a->KeyRepeatMinimum.Rate      = 2;
            a->KeyRepeatMinimum.Delay     = 250;
            a->KeyRepeatMaximum.Rate      = 30;
            a->KeyRepeatMaximum.Delay     = 1000;

            len = sizeof(*a);
            break;
        }

        case IOCTL_MOUSE_QUERY_ATTRIBUTES: {

            MOUSE_ATTRIBUTES *a =
                (MOUSE_ATTRIBUTES *)irp->AssociatedIrp.SystemBuffer;

            if (sl->Parameters.DeviceIoControl.OutputBufferLength
                    < sizeof(*a)) {
                st = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            memset(a, 0, sizeof(*a));
            a->MouseIdentifier      = 2;            /* Radmaus */
            a->NumberOfButtons      = 3;
            a->SampleRate           = 100;
            a->InputDataQueueLength = 32 * sizeof(MOUSE_INPUT_DATA);

            len = sizeof(*a);
            break;
        }

        case IOCTL_INTERNAL_KEYBOARD_ENABLE:
        case IOCTL_INTERNAL_KEYBOARD_DISABLE:
        case IOCTL_INTERNAL_MOUSE_ENABLE:
        case IOCTL_INTERNAL_MOUSE_DISABLE:

            /*  kbdclass schickt das beim Oeffnen des Klassengeraets
             *  (KBDCLASS.C 1290).  Bei uns gibt es nichts an- oder
             *  abzuschalten -- der Uebertragungsfaden laeuft, solange
             *  das Geraet angehaengt ist.  Ein Fehler hier bringt einen
             *  Eintrag ins Systemprotokoll und nichts Gutes.  */

            break;

        case IOCTL_KEYBOARD_QUERY_TYPEMATIC: {

            KEYBOARD_TYPEMATIC_PARAMETERS *t =
                (KEYBOARD_TYPEMATIC_PARAMETERS *)irp->AssociatedIrp.SystemBuffer;

            if (sl->Parameters.DeviceIoControl.OutputBufferLength < sizeof(*t)) {
                st = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            *t = ntu_typematik;
            len = sizeof(*t);
            break;
        }

        case IOCTL_KEYBOARD_SET_TYPEMATIC: {

            KEYBOARD_TYPEMATIC_PARAMETERS *t =
                (KEYBOARD_TYPEMATIC_PARAMETERS *)irp->AssociatedIrp.SystemBuffer;

            if (sl->Parameters.DeviceIoControl.InputBufferLength < sizeof(*t)) {
                st = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            /*  In die Grenzen aus QUERY_ATTRIBUTES zwingen, statt
             *  abzulehnen -- wie i8042prt es tut.  */

            ntu_typematik.Rate  = (t->Rate  < 2)    ? 2    :
                                  (t->Rate  > 30)   ? 30   : t->Rate;
            ntu_typematik.Delay = (t->Delay < 250)  ? 250  :
                                  (t->Delay > 1000) ? 1000 : t->Delay;
            ntu_printf("ukbd: Wiederholung %u/s nach %u ms (von kbdclass)\n",
                       ntu_typematik.Rate, ntu_typematik.Delay);
            break;
        }

        case IOCTL_KEYBOARD_SET_INDICATORS:
#ifdef NTU_EINGABE_PROTOKOLL
            ntu_printf("ukbd: IOCTL SET_INDICATORS (Laenge %lu)\n",
                       sl->Parameters.DeviceIoControl.InputBufferLength);
#endif

            /*  Nie mit Fehler antworten -- ein Fehler hier laesst kbdclass
             *  die Anmeldung abbrechen, und dann gibt es gar keine
             *  Tastatur.  Ein zu kurzer Puffer wird still uebergangen.  */

            if (sl->Parameters.DeviceIoControl.InputBufferLength >=
                sizeof(KEYBOARD_INDICATOR_PARAMETERS)) {
                ntu_kbd_leuchten(((PKEYBOARD_INDICATOR_PARAMETERS)
                                  irp->AssociatedIrp.SystemBuffer)->LedFlags);
            }
            break;

        case IOCTL_KEYBOARD_QUERY_INDICATORS:
            if (sl->Parameters.DeviceIoControl.OutputBufferLength >=
                sizeof(KEYBOARD_INDICATOR_PARAMETERS)) {
                PKEYBOARD_INDICATOR_PARAMETERS q =
                    (PKEYBOARD_INDICATOR_PARAMETERS)irp->AssociatedIrp.SystemBuffer;
                q->UnitId   = 0;
                q->LedFlags = ntu_kbd_leuchten_jetzt();
                len = sizeof(KEYBOARD_INDICATOR_PARAMETERS);
            }
            break;

        default:
            st = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    irp->IoStatus.Status      = st;
    irp->IoStatus.Information = len;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/*  Lesen, Schreiben und Leeren gehen an das Plattengeraet, wenn es eines
 *  ist.  Fuer alle anderen Geraete dieses Treibers bleibt es beim
 *  Durchwinken -- das aber darf fuer eine Platte NIE gelten: ein
 *  "Erfolg, 0 Byte" auf IRP_MJ_READ liesse das Dateisystem Nullen lesen
 *  und Schreiben fuer gelungen halten.  */

static NTSTATUS ntu_port_pass(PDEVICE_OBJECT, PIRP);

static NTSTATUS
ntu_port_rw (PDEVICE_OBJECT dev, PIRP irp)
{
    ntu_port *p = (ntu_port *)dev->DeviceExtension;

    if (p->Kind == NTU_PORT_PLATTE) {
        return ntum_irp(dev, irp);
    }

    if (p->Kind == NTU_PORT_DRUCK) {
        return ntup_irp(dev, irp);
    }

    if (IoGetCurrentIrpStackLocation(irp)->MajorFunction != IRP_MJ_FLUSH_BUFFERS) {
        irp->IoStatus.Status      = STATUS_INVALID_DEVICE_REQUEST;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    return ntu_port_pass(dev, irp);
}

/*  Oeffnen, Schliessen, Aufraeumen: der Drucker achtet dabei auf den
 *  einen schreibenden Griff und bricht beim Aufraeumen die Anfragen des
 *  schliessenden Griffs ab.  Alle anderen Geraete: durchwinken wie
 *  bisher.  */

static NTSTATUS
ntu_port_aufraeumen (PDEVICE_OBJECT dev, PIRP irp)
{
    if (((ntu_port *)dev->DeviceExtension)->Kind == NTU_PORT_DRUCK) {
        return ntup_irp(dev, irp);
    }
    return ntu_port_pass(dev, irp);
}

static NTSTATUS
ntu_port_pass (PDEVICE_OBJECT dev, PIRP irp)
{
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}


/* ====================================================================
 *  Anlegen
 *
 *  Der Name muss ein Geraetename sein, kein symbolischer.  kbdclass
 *  sucht \Device\KeyboardPortN; ein Verweis unter \DosDevices hilft
 *  nicht.
 * ==================================================================== */

static void ntu_port_delete(PDEVICE_OBJECT);

static int
ntu_port_create (ULONG kind, void *softc, NTU_KBD_CONNECT *conn,
                 PDEVICE_OBJECT *out)
{
    UNICODE_STRING name;
    PDEVICE_OBJECT dev = 0;
    NTSTATUS       st;
    ntu_port      *p;
    ULONG          i;

    if (NtuDriverObject == 0) {
        ntu_printf("usb: kein Treiberobjekt -- Anschluss nicht anlegbar\n");
        return ENXIO;
    }

    /*  Ab 0, nicht ab 1.  kbdclass baut die Namen aus der Anzahl der
     *  DEVICEMAP-Eintraege zusammen und verlangt sie luckenlos ab 0.
     *  Ist i8042prt da, ist die 0 belegt und wir bekommen die 1; ist es
     *  nicht da, muessen wir die 0 nehmen.  STATUS_OBJECT_NAME_COLLISION
     *  ist dabei der Normalfall, nicht der Fehlerfall.  */

    for (i = 0; i < 8; i += 1) {

        WCHAR  puffer[40];
        PCWSTR muster = (kind == 0) ? L"\\Device\\KeyboardPort%u"
                                    : L"\\Device\\PointerPort%u";

        ntu_wsprintf(puffer, sizeof(puffer) / sizeof(WCHAR), muster, i);
        RtlInitUnicodeString(&name, puffer);

        st = IoCreateDevice(NtuDriverObject, sizeof(ntu_port), &name,
                            (kind == 0) ? FILE_DEVICE_KEYBOARD
                                        : FILE_DEVICE_MOUSE,
                            0, FALSE, &dev);

        if (NT_SUCCESS(st)) {
            break;
        }

        dev = 0;
    }

    if (dev == 0) {
        ntu_printf("usb: kein freier Anschlussname\n");
        return ENXIO;
    }

    p        = (ntu_port *)dev->DeviceExtension;
    p->Kind  = kind;
    p->Softc = softc;
    p->Conn  = conn;

    conn->ClassService      = 0;
    conn->ClassDeviceObject = 0;

    /*  Gepufferte Ein-/Ausgabe: die Abfrage-IOCTLs sind METHOD_BUFFERED,
     *  die Anmeldung METHOD_NEITHER.  Beides geht mit DO_BUFFERED_IO --
     *  METHOD_NEITHER umgeht die Kennzeichnung ohnehin.  */

    dev->Flags |= DO_BUFFERED_IO;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;

    /*  Der Eintrag im DEVICEMAP.  Ohne ihn zaehlt kbdclass uns nicht mit
     *  und oeffnet uns nie -- der Geraetename allein genuegt nicht.
     *  Wertname ist der volle Geraetename, Inhalt der Registrierungspfad
     *  unseres Dienstes; so macht es i8042prt (I8042DEP.C 1223).  */

    {
        WCHAR basis[20];
        WCHAR voll[40];

        ntu_wsprintf(basis, sizeof(basis) / sizeof(WCHAR),
                     (kind == 0) ? L"KeyboardPort" : L"PointerPort", 0);
        ntu_wsprintf(voll, sizeof(voll) / sizeof(WCHAR),
                     (kind == 0) ? L"\\Device\\KeyboardPort%u"
                                 : L"\\Device\\PointerPort%u", i);

        st = RtlWriteRegistryValue(RTL_REGISTRY_DEVICEMAP, basis, voll,
                                   REG_SZ, ntu_regpfad, ntu_regpfad_len);

        if (!NT_SUCCESS(st)) {

            /*  Kein Abbruch: das Geraet steht, es wird nur nicht
             *  geoeffnet.  Der Hinweis gehoert aber deutlich in den
             *  Bericht, weil es sonst aussieht, als lieferte die
             *  Tastatur keine Anschlaege.  */

            ntu_printf("usb: DEVICEMAP-Eintrag fehlgeschlagen (%08x) -- "
                       "kbdclass wird diesen Anschluss nicht oeffnen\n", st);
        }

        p->Suffix = i;
    }

    *out = dev;
    ntu_printf("usb: Anschluss %s%u angelegt\n",
               (kind == 0) ? "KeyboardPort" : "PointerPort", i);
    return 0;
}

/* ====================================================================
 *  Abbauen
 *
 *  Erst aus dem DEVICEMAP heraus, dann das Geraet weg.  Bleibt der
 *  Eintrag stehen, zaehlt kbdclass beim naechsten Start einen Anschluss
 *  mit, den es nicht gibt -- und weil es die Namen aus der Anzahl baut,
 *  faellt das auf einen anderen Anschluss zurueck.
 * ==================================================================== */

static void
ntu_port_delete (PDEVICE_OBJECT dev)
{
    ntu_port *p;
    WCHAR     basis[20];
    WCHAR     voll[40];

    if (dev == 0) {
        return;
    }

    p = (ntu_port *)dev->DeviceExtension;

    ntu_wsprintf(basis, sizeof(basis) / sizeof(WCHAR),
                 (p->Kind == NTU_PORT_KBD) ? L"KeyboardPort" : L"PointerPort", 0);
    ntu_wsprintf(voll, sizeof(voll) / sizeof(WCHAR),
                 (p->Kind == NTU_PORT_KBD) ? L"\\Device\\KeyboardPort%u"
                                           : L"\\Device\\PointerPort%u",
                 p->Suffix);

    RtlDeleteRegistryValue(RTL_REGISTRY_DEVICEMAP, basis, voll);
    IoDeleteDevice(dev);
}


/* ====================================================================
 *  Die beiden festen Anschluesse
 * ==================================================================== */

static NTU_KBD_CONNECT ntu_kbd_conn;

/*  Tastenwiederholung, fuer alle USB-Tastaturen gemeinsam.
 *
 *  Eine PS/2-Tastatur wiederholt selbst (Typematic); i8042prt reicht das
 *  nur durch, und NT erzeugt keine eigenen Wiederholungen.  Eine
 *  USB-Tastatur wiederholt NIE -- sie meldet gedrueckt und losgelassen.
 *  Die Wiederholung muss also hier entstehen (ntukbd.c), so wie unter
 *  Windows 2000 in kbdhid.sys.
 *
 *  Vorgabe 500 ms / 30 je Sekunde: die Windows-Grundeinstellung
 *  (KeyboardDelay 1, KeyboardSpeed 31).  Sie muss von sich aus stimmen:
 *  mit ConnectMultiplePorts = 1 leitet kbdclass SET_TYPEMATIC nach der
 *  UnitId weiter, und USER schickt es nur fuer Einheit 0 -- die
 *  PS/2-Tastatur.  */

KEYBOARD_TYPEMATIC_PARAMETERS ntu_typematik = { 0, 30, 500 };
static NTU_MOU_CONNECT ntu_mou_conn;
static PDEVICE_OBJECT  ntu_kbd_dev;
static PDEVICE_OBJECT  ntu_mou_dev;
static LONG            ntu_kbd_quellen;
static LONG            ntu_mou_quellen;

NTU_KBD_CONNECT *
ntu_kbd_anmelden (void)
{
    ntu_kbd_quellen += 1;
    ntu_printf("usb: Tastatur %d am Anschluss, kbdclass %s\n",
               ntu_kbd_quellen,
               (ntu_kbd_conn.ClassService != 0) ? "verbunden"
                                                 : "NICHT verbunden");
    return &ntu_kbd_conn;
}

/*  Tasten einspeisen -- fuer Maustasten 4..8.  Geht ueber dieselbe
 *  Verbindung wie die USB-Tastaturen; kbdclass stellt sie beim Booten
 *  her, sie besteht also auch ohne angesteckte USB-Tastatur.  kbdclass
 *  verlangt DISPATCH_LEVEL und schuetzt sich selbst gegen gleichzeitige
 *  Aufrufe von mehreren Quellen.  */

VOID
ntu_kbd_einspeisen (const USHORT *codes, int n, int los)
{
    KEYBOARD_INPUT_DATA k[8];
    ULONG               verbraucht = 0;
    KIRQL               alt;
    int                 i;

    if (ntu_kbd_conn.ClassService == 0 || n <= 0 || n > 8) {
        return;
    }

    for (i = 0; i < n; i += 1) {
        USHORT c = los ? codes[n - 1 - i] : codes[i];

        k[i].UnitId           = 0;
        k[i].MakeCode         = (USHORT)(c & 0xFF);
        k[i].Flags            = (USHORT)((los ? KEY_BREAK : KEY_MAKE) |
                                         ((c & 0x100) ? KEY_E0 : 0));
        k[i].Reserved         = 0;
        k[i].ExtraInformation = 0;
    }

    KeRaiseIrql(DISPATCH_LEVEL, &alt);
    ntu_kbd_conn.ClassService(ntu_kbd_conn.ClassDeviceObject, &k[0], &k[n],
                              &verbraucht);
    KeLowerIrql(alt);
}

NTU_MOU_CONNECT *
ntu_mou_anmelden (void)
{
    ntu_mou_quellen += 1;
    ntu_printf("usb: Maus %d am Anschluss, mouclass %s\n",
               ntu_mou_quellen,
               (ntu_mou_conn.ClassService != 0) ? "verbunden"
                                                 : "NICHT verbunden");
    return &ntu_mou_conn;
}

void ntu_kbd_abmelden (void) { ntu_kbd_quellen -= 1; }
void ntu_mou_abmelden (void) { ntu_mou_quellen -= 1; }

void
ntu_port_ende (void)
{
    ntu_port_delete(ntu_kbd_dev);
    ntu_port_delete(ntu_mou_dev);
    ntu_kbd_dev = 0;
    ntu_mou_dev = 0;
}

/* ====================================================================
 *  Einhaengen
 *
 *  Wird aus DriverEntry gerufen, bevor irgendein Geraet anhaengt.
 * ==================================================================== */

VOID
ntu_port_init (PDRIVER_OBJECT drv, PUNICODE_STRING regpfad)
{
    ULONG i;

    NtuDriverObject = drv;

    /*  Den Registrierungspfad kopieren: der UNICODE_STRING aus
     *  DriverEntry gehoert dem Aufrufer und ist nach der Rueckkehr
     *  nicht mehr gueltig.  */

    ntu_regpfad_len = 0;

    if (regpfad != 0 && regpfad->Buffer != 0) {

        USHORT n = regpfad->Length;

        if (n > sizeof(ntu_regpfad) - sizeof(WCHAR)) {
            n = sizeof(ntu_regpfad) - sizeof(WCHAR);
        }

        memcpy(ntu_regpfad, regpfad->Buffer, n);
        ntu_regpfad[n / sizeof(WCHAR)] = 0;
        ntu_regpfad_len = (USHORT)(n + sizeof(WCHAR));
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i += 1) {
        drv->MajorFunction[i] = ntu_port_pass;
    }

    drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = ntu_port_ioctl;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL]          = ntu_port_ioctl;
    drv->MajorFunction[IRP_MJ_READ]                    = ntu_port_rw;
    drv->MajorFunction[IRP_MJ_WRITE]                   = ntu_port_rw;
    drv->MajorFunction[IRP_MJ_FLUSH_BUFFERS]           = ntu_port_rw;
    drv->MajorFunction[IRP_MJ_CLEANUP]                 = ntu_port_aufraeumen;
    drv->MajorFunction[IRP_MJ_CREATE]                  = ntu_port_aufraeumen;
    drv->MajorFunction[IRP_MJ_CLOSE]                   = ntu_port_aufraeumen;

    /*  Jetzt, nicht spaeter.  Damit das reicht, muss der Dienst in der
     *  Gruppe "Keyboard Port" stehen -- die laedt vor "Pointer Class"
     *  und "Keyboard Class".  Ohne Gruppe laedt er nach ihnen, und dann
     *  hilft auch das fruehe Anlegen nichts mehr.  */

    memset(&ntu_kbd_conn, 0, sizeof(ntu_kbd_conn));
    memset(&ntu_mou_conn, 0, sizeof(ntu_mou_conn));
    ntu_port_create(NTU_PORT_KBD,  0, &ntu_kbd_conn, &ntu_kbd_dev);
    ntu_port_create(NTU_PORT_MAUS, 0, (NTU_KBD_CONNECT *)&ntu_mou_conn,
                    &ntu_mou_dev);
}
