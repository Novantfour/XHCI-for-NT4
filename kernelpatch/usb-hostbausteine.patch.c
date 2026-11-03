/*
 *  usb-hostbausteine.patch.c
 *
 *  Zwei Ergaenzungen fuer den p08c-Baum.  Sie schreiben keinen Treiber --
 *  sie beantworten die Frage, die vor dem Treiber steht: welche
 *  USB-Hostbausteine stecken auf dem Brett, welcher Bauart sind sie, und
 *  wo liegen ihre Anschluesse.
 *
 *  Der bisherige Suchlauf in IopScanPciBus merkt sich nur Hersteller,
 *  Baustein und Untersystem.  Fuer USB fehlt genau das, was zaehlt:
 *  Klasse, Schnittstellenkennung, Anschlussadresse, Unterbrechung.
 *
 *      Klasse 0x0C, Unterklasse 0x03 ist USB.  Die Schnittstellenkennung
 *      sagt die Bauart:
 *
 *          0x00   UHCI   USB 1.1, Anschluesse im E/A-Raum, BAR4
 *          0x10   OHCI   USB 1.1, Anschluesse im Speicher, BAR0
 *          0x20   EHCI   USB 2.0, BAR0
 *          0x30   xHCI   USB 3.x, BAR0
 *          0x80   sonst  herstellereigen
 *          0xFE   nur Geraet, kein Hostbaustein
 *
 *  Zusaetzlich aufgenommen:
 *
 *      SBRN     Konfig 0x60, "Serial Bus Release Number".  0x10 = 1.0,
 *               0x20 = 2.0, 0x30 = 3.0.  Ein EHCI ohne SBRN 0x20 ist ein
 *               Fehler im Brett und spart spaeter einen halben Tag.
 *      LegSup   nur UHCI, Konfig 0xC0.  Steht dort Bit 0x8000 oder sind
 *               die Ableitungsbits gesetzt, emuliert das BIOS noch eine
 *               PS/2-Tastatur ueber diesen Baustein.  Wer ihn uebernimmt,
 *               ohne das abzuschalten, bekommt einen Baustein, den der
 *               SMI-Handler hinter seinem Ruecken zuruecksetzt.
 *      Command  Konfig 0x04.  Bit 0x02 (Speicher) bzw. 0x01 (E/A) und
 *               Bit 0x04 (Busmeister) muessen an sein, sonst antwortet
 *               der Baustein auf nichts.
 */


/* ======================================================================
 *  1.  private/ntos/io/ioinit.c
 *
 *  Einzufuegen unmittelbar hinter der Zeile
 *
 *      static ULONG      IopPciPresentCount = 0;
 * ====================================================================== */

#define IOUSBHC 16

typedef struct _IOP_USB_HC {
    UCHAR   Bus;
    UCHAR   Slot;
    UCHAR   ProgIf;
    UCHAR   Irq;
    USHORT  Vendor;
    USHORT  Device;
    USHORT  SubVendor;
    USHORT  SubSystem;
    ULONG   Bar;
    USHORT  Command;
    UCHAR   Sbrn;
    UCHAR   LegSupHi;
    UCHAR   LegSupLo;
} IOP_USB_HC;

IOP_USB_HC IopUsbHc[IOUSBHC];
ULONG      IopUsbHcCount = 0;


/* ----------------------------------------------------------------------
 *  In IopScanPciBus, unmittelbar hinter dem Block
 *
 *      if (IopPciPresentCount < IOPCIDEV) { ... }
 *
 *  und noch vor
 *
 *      if (fn == 0 && (cfg.HeaderType & 0x80) == 0) break;
 * ---------------------------------------------------------------------- */

                if (cfg.BaseClass == 0x0C &&
                    cfg.SubClass  == 0x03 &&
                    (cfg.HeaderType & 0x7F) == 0 &&
                    IopUsbHcCount < IOUSBHC) {

                    IOP_USB_HC* u = &IopUsbHc[IopUsbHcCount];

                    IopUsbHcCount += 1;

                    u->Bus       = (UCHAR)bus;
                    u->Slot      = (UCHAR)slot;
                    u->ProgIf    = cfg.ProgIf;
                    u->Irq       = cfg.u.type0.InterruptLine;
                    u->Vendor    = cfg.VendorID;
                    u->Device    = cfg.DeviceID;
                    u->SubVendor = cfg.u.type0.SubVendorID;
                    u->SubSystem = cfg.u.type0.SubSystemID;
                    u->Command   = cfg.Command;
                    u->Sbrn      = 0;
                    u->LegSupHi  = 0;
                    u->LegSupLo  = 0;

                    //
                    //  UHCI liegt im E/A-Raum und benutzt BAR4, alles
                    //  andere liegt im Speicher und benutzt BAR0.
                    //

                    u->Bar = (cfg.ProgIf == 0x00)
                                ? cfg.u.type0.BaseAddresses[4]
                                : cfg.u.type0.BaseAddresses[0];

                    //
                    //  DeviceSpecific deckt Konfig 0x40 bis 0xFF ab.  Nur
                    //  lesen, wenn HalGetBusData auch so weit gekommen
                    //  ist -- sonst steht dort Muell vom Stapel.
                    //

                    if (got >= sizeof(PCI_COMMON_CONFIG)) {
                        PUCHAR spec = (PUCHAR)cfg.DeviceSpecific;

                        u->Sbrn = spec[0x60 - 0x40];

                        if (cfg.ProgIf == 0x00) {
                            u->LegSupLo = spec[0xC0 - 0x40];
                            u->LegSupHi = spec[0xC1 - 0x40];
                        }
                    }
                }


/* ======================================================================
 *  2.  private/ntos/ke/i386/kiserial.c
 *
 *  a) Bei den uebrigen extern-Zeilen, neben IopDevMapDevices:
 * ====================================================================== */

typedef struct _IOP_USB_HC {
    UCHAR   Bus;
    UCHAR   Slot;
    UCHAR   ProgIf;
    UCHAR   Irq;
    USHORT  Vendor;
    USHORT  Device;
    USHORT  SubVendor;
    USHORT  SubSystem;
    ULONG   Bar;
    USHORT  Command;
    UCHAR   Sbrn;
    UCHAR   LegSupHi;
    UCHAR   LegSupLo;
} IOP_USB_HC;

extern IOP_USB_HC IopUsbHc[16];
extern ULONG      IopUsbHcCount;


/* ----------------------------------------------------------------------
 *  b) Kurzer Helfer, neben KiLogHex.  KiLogHex schreibt immer acht
 *     Stellen; fuer eine Tabelle sind zwei oder vier genug.
 * ---------------------------------------------------------------------- */

VOID
KiLogHexN (
    IN ULONG Value,
    IN ULONG Digits
    )
{
    CHAR  d[9];
    ULONG i;

    if (Digits == 0 || Digits > 8) {
        Digits = 8;
    }

    for (i = 0; i < Digits; i += 1) {
        ULONG n = (Value >> ((Digits - 1 - i) * 4)) & 0xF;

        d[i] = (CHAR)(n < 10 ? '0' + n : 'a' + (n - 10));
    }

    d[Digits] = '\0';
    KiLogStr(d);
}


/* ----------------------------------------------------------------------
 *  c) Neuer Abschnitt im Bericht, unmittelbar hinter dem Block
 *     "Geraetezuordnung (DeviceMap2)".
 * ---------------------------------------------------------------------- */

    KiLogStr("\r\nUSB-Hostbausteine\r\n");
    KiLogPair("  gefunden             ", IopUsbHcCount);

    if (IopUsbHcCount != 0) {

        KiLogStr("    bus:slot  bauart  hst  bstn  unter-sys  "
                 "anschluss  irq  sbrn  cmd   legsup\r\n");
        KiLogStr("    ------------------------------------------"
                 "-------------------------------------\r\n");

        for (i = 0; i < IopUsbHcCount && i < 16; i += 1) {

            IOP_USB_HC* u = &IopUsbHc[i];
            PCHAR       art;

            switch (u->ProgIf) {
                case 0x00: art = "uhci  "; break;
                case 0x10: art = "ohci  "; break;
                case 0x20: art = "ehci  "; break;
                case 0x30: art = "xhci  "; break;
                case 0xFE: art = "geraet"; break;
                default:   art = "eigen "; break;
            }

            KiLogStr("     ");
            KiLogHexN(u->Bus, 2);
            KiLogStr(":");
            KiLogHexN(u->Slot, 2);
            KiLogStr("     ");
            KiLogStr(art);
            KiLogStr("  ");
            KiLogHexN(u->Vendor, 4);
            KiLogStr(" ");
            KiLogHexN(u->Device, 4);
            KiLogStr("  ");
            KiLogHexN(u->SubVendor, 4);
            KiLogStr(":");
            KiLogHexN(u->SubSystem, 4);
            KiLogStr("  ");
            KiLogHex(u->Bar);
            KiLogStr("   ");
            KiLogHexN(u->Irq, 2);
            KiLogStr("    ");
            KiLogHexN(u->Sbrn, 2);
            KiLogStr("   ");
            KiLogHexN(u->Command, 4);
            KiLogStr("  ");
            KiLogHexN(u->LegSupHi, 2);
            KiLogHexN(u->LegSupLo, 2);
            KiLogStr("\r\n");
        }

        //
        //  Die drei Zeilen, die den Bericht beim Lesen sofort brauchbar
        //  machen.  Sie sagen, ob ueberhaupt etwas zu tun ist.
        //

        KiLogStr("\r\n    lesehilfe\r\n");
        KiLogStr("      anschluss endet auf 1   e/a-raum, uhci\r\n");
        KiLogStr("      anschluss ist 00000000  bios hat nichts "
                 "vergeben -- baustein unbrauchbar\r\n");
        KiLogStr("      cmd ohne bit 0004       kein busmeister, "
                 "dma wird nicht laufen\r\n");
        KiLogStr("      legsup mit bit 8000     bios emuliert noch "
                 "ps/2 ueber diesen baustein\r\n");
    }
