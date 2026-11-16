/*
 *  nt/ntintr.c -- die Unterbrechung im Faden, und was dazugehoert.
 *
 *  Das ist die Stelle, an der sich FreeBSD und NT nicht decken, und die
 *  einzige, an der die Unterlage etwas tut, was der Treiber nicht sieht.
 *
 *  FreeBSDs USB-Unterbrechung ist ein ithread: sie laeuft im
 *  Prozesskontext, darf Mutexe nehmen und auf Bedingungsvariablen
 *  warten.  Der Stapel macht davon reichlich Gebrauch.  Unter NT laeuft
 *  ein ISR auf DIRQL; dort ist beides ein Bugcheck.
 *
 *  Nachgebaut wird deshalb genau der ithread:
 *
 *      ntu_isr            DIRQL.  Prueft, ob es unser Baustein war,
 *                         stellt die Quelle am Baustein ab, stellt
 *                         einen DPC ein.  Sonst nichts.
 *      ntu_dpc            DISPATCH_LEVEL.  Setzt ein Ereignis.
 *      ntu_ithread_body   PASSIVE_LEVEL.  Ruft den echten Handler und
 *                         gibt die Quelle wieder frei.
 *
 *  Warum der ISR die Quelle abstellen MUSS: die PCI-Leitung ist
 *  pegelgesteuert.  Gibt der ISR TRUE zurueck, ohne dass der Baustein
 *  aufhoert zu ziehen, wird er sofort wieder gerufen -- und zwar bevor
 *  der Faden ueberhaupt eingeplant wird.  Der Rechner steht dann in
 *  einem Sturm, der wie ein Haenger aussieht und keiner ist.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <machine/bus.h>
#include <dev/pci/pcireg.h>
#include <ober/ntusbioctl.h>

extern NTU_ZAEHLER ntu_zaehler;


/* ====================================================================
 *  Abstellen und Freigeben
 *
 *  Welches Register das ist, steht in den Bausteinhandbuechern und in
 *  den FreeBSD-Treibern; nachgelesen, nicht aus dem Gedaechtnis:
 *
 *      UHCI   USBINTR    E/A + 0x04, 16 Bit.  Kein Set/Clear-Paar --
 *                        der alte Wert muss gemerkt werden.
 *      OHCI   HcInterruptDisable  0x14, HcInterruptEnable  0x10.
 *                        Zwei getrennte Register, beide nur schreibend,
 *                        beide wirken auf dieselben Bits.  Das ist der
 *                        saubere Fall: kein Lesen, kein Zurueckschreiben,
 *                        kein Wettlauf mit dem Treiber.
 *      EHCI   USBINTR    oper + 0x08, 32 Bit, oper = cap + CAPLENGTH.
 *                        Wie UHCI: merken und zuruecklegen.
 *      xHCI   IMAN.IE    Laufzeitblock + 0x20, Bit 1.
 *                        Der Laufzeitblock steht in RTSOFF (cap 0x18,
 *                        untere fuenf Bit gehoeren nicht zur Adresse).
 *                        IMAN.IP (Bit 0) ist write-1-to-clear: eine
 *                        Null dort laesst es stehen, eine Eins loescht
 *                        es.  Deshalb wird beim Sperren 0x3 abgezogen
 *                        und beim Freigeben Bit 0 ausdruecklich nicht
 *                        gesetzt -- sonst quittiert die Unterlage eine
 *                        Unterbrechung, die der Treiber noch gar nicht
 *                        gesehen hat.
 *
 *                        USBCMD.INTE waere einfacher, schaltet aber den
 *                        ganzen Baustein ab und damit auch Unterbrecher,
 *                        die uns nicht gehoeren.
 *
 *  Bei UHCI und EHCI liegt ein Wettlauf: schreibt der Treiber USBINTR,
 *  waehrend wir den gemerkten Wert halten, legen wir den alten zurueck.
 *  Praktisch passiert das nicht, weil der Treiber USBINTR nur beim
 *  Anhaengen und beim Abhaengen anfasst -- nachgesehen in uhci_init und
 *  ehci_init, nicht angenommen.  Es steht hier trotzdem, weil es die
 *  Art Annahme ist, die sich still aendert.
 * ==================================================================== */

#define OHCI_INTERRUPT_ENABLE   0x10
#define OHCI_INTERRUPT_DISABLE  0x14
#define OHCI_MIE                0x80000000u
#define UHCI_INTR               0x04
#define EHCI_CAPLENGTH          0x00
#define EHCI_USBINTR            0x08
#define XHCI_RTSOFF             0x18
#define XHCI_IMAN               0x20    /* Laufzeitblock + 0x20 */
#define XHCI_IMAN_IP            0x01
#define XHCI_IMAN_IE            0x02

/* ====================================================================
 *  Ist die Unterbrechung ueberhaupt unsere?
 *
 *  Auf einer geteilten PCI-Leitung ist das keine Hoeflichkeit, sondern
 *  Pflicht: ein falsches TRUE nimmt dem Nachbarn seine Unterbrechung
 *  weg, und der wartet dann fuer immer.
 *
 *  FreeBSDs Hostbaustein-Treiber tragen KEINEN Filter ein -- ihr
 *  Handler prueft selbst und tut nichts, wenn nichts anliegt.  Unter
 *  FreeBSD ist das in Ordnung, weil dort jeder Handler der Kette
 *  gerufen wird.  NT fragt stattdessen jeden ISR der Reihe nach, und
 *  der erste, der TRUE sagt, beendet die Kette.  Deshalb steht der
 *  Filter hier und nicht im Treiber.
 *
 *  Gelesen wird ausschliesslich der Zustand, nichts wird quittiert --
 *  das Quittieren ist Sache des Handlers, und ein ISR, der es vorweg
 *  tut, laesst den Handler ins Leere laufen.
 *
 *      UHCI   USBSTS      E/A + 0x02, 16 Bit; irgendein Bit gesetzt
 *      OHCI   HcInterruptStatus 0x0C UND HcInterruptEnable 0x10
 *      EHCI   USBSTS oper+0x04 UND USBINTR oper+0x08
 *      xHCI   USBSTS.EINT oper+0x04 ODER IMAN.IP
 *
 *  Bei OHCI und EHCI zaehlt nur, was auch freigegeben ist -- ein
 *  Zustandsbit ohne zugehoerige Freigabe zieht die Leitung nicht.
 * ==================================================================== */

#define OHCI_INTERRUPT_STATUS   0x0C
#define EHCI_USBSTS             0x04
#define UHCI_STS                0x02
#define XHCI_USBSTS             0x04
#define XHCI_STS_EINT           0x08

static int
ntu_intr_erkannt (ntu_intr *i)
{
    switch (i->ProgIf) {

        case PCIP_SERIALBUS_USB_UHCI:
            return (ntu_r2(i->Tag, i->Handle, UHCI_STS) & 0x3F) != 0;

        case PCIP_SERIALBUS_USB_OHCI: {

            ULONG sts = ntu_r4(i->Tag, i->Handle, OHCI_INTERRUPT_STATUS);
            ULONG ena = ntu_r4(i->Tag, i->Handle, OHCI_INTERRUPT_ENABLE);

            /*  MIE (0x80000000) ist der Hauptschalter und kein Ereignis;
             *  er darf die Erkennung nicht allein tragen.  */

            return (sts & ena & 0x7FFFFFFFul) != 0;
        }

        case PCIP_SERIALBUS_USB_EHCI: {

            ULONG sts = ntu_r4(i->Tag, i->Handle, i->OpBase + EHCI_USBSTS);
            ULONG ena = ntu_r4(i->Tag, i->Handle, i->OpBase + EHCI_USBINTR);

            return (sts & ena & 0x3F) != 0;
        }

        case PCIP_SERIALBUS_USB_XHCI: {

            ULONG sts = ntu_r4(i->Tag, i->Handle, i->OpBase + XHCI_USBSTS);
            ULONG man = ntu_r4(i->Tag, i->Handle, i->Iman);

            return ((sts & XHCI_STS_EINT) != 0) || ((man & 0x01) != 0);
        }

        default:

            /*  Unbekannte Bauart: lieber annehmen als verlieren.  Sie
             *  wird ohnehin nicht angehaengt.  */

            return 1;
    }
}

VOID
ntu_intr_mask (ntu_intr *i)
{
    switch (i->ProgIf) {

        case PCIP_SERIALBUS_USB_UHCI:
            i->Saved = ntu_r2(i->Tag, i->Handle, UHCI_INTR);
            ntu_w2(i->Tag, i->Handle, UHCI_INTR, 0);
            break;

        case PCIP_SERIALBUS_USB_OHCI:
            ntu_w4(i->Tag, i->Handle, OHCI_INTERRUPT_DISABLE, OHCI_MIE);
            break;

        case PCIP_SERIALBUS_USB_EHCI:
            i->Saved = ntu_r4(i->Tag, i->Handle, i->OpBase + EHCI_USBINTR);
            ntu_w4(i->Tag, i->Handle, i->OpBase + EHCI_USBINTR, 0);

            /*  USBSTS mit NULL beschreiben.  Die Statusbits dort sind
             *  write-1-to-clear; eine Null aendert nach der EHCI-Norm
             *  nichts.  Auf echtem Silizium ist die Leitung ohnehin die
             *  Verknuepfung aus Status und Freigabe und faellt mit
             *  USBINTR = 0 sofort.
             *
             *  QEMU rechnet den Pegel aber nur beim Schreiben auf USBSTS
             *  neu, nicht beim Schreiben auf USBINTR (hcd-ehci.c,
             *  ehci_opreg_write).  Ohne diese Zeile blieb die Leitung dort
             *  oben, bis der Faden quittierte -- und in diesem Fenster
             *  meldete der ISR sieben Millionen Mal "gesperrt, nicht
             *  meins".  Die Null kostet einen Buszugriff und macht die
             *  Sperre unabhaengig davon, wie genau eine Umgebung die Norm
             *  nimmt.  */

            ntu_w4(i->Tag, i->Handle, i->OpBase + EHCI_USBSTS, 0);
            break;

        case PCIP_SERIALBUS_USB_XHCI:
            i->Saved = ntu_r4(i->Tag, i->Handle, i->Iman);
            ntu_w4(i->Tag, i->Handle, i->Iman,
                   i->Saved & ~(XHCI_IMAN_IE | XHCI_IMAN_IP));
            break;

        default:
            break;
    }
}

VOID
ntu_intr_unmask (ntu_intr *i)
{
    switch (i->ProgIf) {

        case PCIP_SERIALBUS_USB_UHCI:
            ntu_w2(i->Tag, i->Handle, UHCI_INTR, (uint16_t)i->Saved);
            break;

        case PCIP_SERIALBUS_USB_OHCI:
            ntu_w4(i->Tag, i->Handle, OHCI_INTERRUPT_ENABLE, OHCI_MIE);
            break;

        case PCIP_SERIALBUS_USB_EHCI:
            ntu_w4(i->Tag, i->Handle, i->OpBase + EHCI_USBINTR, i->Saved);

            /*  Dasselbe beim Freigeben, aus dem umgekehrten Grund: liegt
             *  schon wieder ein Ereignis an, soll die Leitung JETZT
             *  steigen und nicht erst beim naechsten Rahmenwechsel.  */

            ntu_w4(i->Tag, i->Handle, i->OpBase + EHCI_USBSTS, 0);
            break;

        case PCIP_SERIALBUS_USB_XHCI:
            ntu_w4(i->Tag, i->Handle, i->Iman,
                   (i->Saved & ~XHCI_IMAN_IP) | XHCI_IMAN_IE);
            break;

        default:
            break;
    }
}


/* ====================================================================
 *  Die drei Stufen
 * ==================================================================== */

ntu_spur ntu_spur_ring[NTU_SPUR_TIEFE];
ULONG    ntu_spur_kopf;

BOOLEAN
ntu_isr (PKINTERRUPT unused, PVOID ctx)
{
    ntu_intr *i = (ntu_intr *)ctx;

    /*  Vor dem Filter aufzeichnen, nicht danach.  Abgewiesene
     *  Unterbrechungen sind die interessanteren: sie sagen, ob auf der
     *  Leitung ein Nachbar sitzt -- und ob wir ihm frueher seine
     *  weggenommen haben.  */

    if (i->ProgIf == PCIP_SERIALBUS_USB_XHCI) {

        ntu_spur *sp = &ntu_spur_ring[ntu_spur_kopf % NTU_SPUR_TIEFE];

        sp->Iman   = ntu_r4(i->Tag, i->Handle, i->Iman);
        sp->Usbsts = ntu_r4(i->Tag, i->Handle, i->OpBase + 0x04);
        sp->Tick   = (ULONG)ticks;
        ntu_spur_kopf += 1;
    }

    /*  Der Filter sagt, ob es unser Baustein war.  Bei geteilter Leitung
     *  ist das Pflicht, nicht Hoeflichkeit: ein falsches TRUE nimmt dem
     *  richtigen Treiber seine Unterbrechung weg.  Auf SB700 haengen
     *  OHCI.0, OHCI.1 und EHCI.2 an derselben Leitung.
     *
     *  FreeBSDs USB-Treiber tragen keinen Filter ein -- ihr Handler
     *  prueft selbst.  Ohne Filter muessen wir deshalb immer TRUE
     *  melden und uns darauf verlassen, dass der Handler nichts tut,
     *  wenn nichts anliegt.  Das ist korrekt, kostet aber bei geteilter
     *  Leitung jedes Mal einen Fadenwechsel.  */

    /*  Solange gesperrt ist, kann die Meldung nicht von uns sein: unsere
     *  Quelle steht ab, die Leitung zieht also ein Nachbar.  Ohne diese
     *  Abfrage wuerde der Filter bei xHCI ein noch stehendes IP sehen,
     *  TRUE melden und ein zweites Mal sperren -- wobei der gemerkte
     *  Wert durch den bereits gesperrten ueberschrieben wird.
     *
     *  Genau diese Sorte Fehler hat schon einmal die IRQL oben gelassen;
     *  einmal reicht.  */

    if (i->Gesperrt != 0) {
        InterlockedIncrement((LONG *)&ntu_zaehler.Fremdunterbr);
        i->Fremd += 1;
        i->FremdGesp += 1;
        return FALSE;
    }

    /*  Erst der Filter des Treibers, falls er einen eingetragen hat;
     *  sonst unser eigener, der den Baustein selbst fragt.  */

    if (i->Filter != 0) {

        if (i->Filter(i->Arg) == 0) {
            InterlockedIncrement((LONG *)&ntu_zaehler.Fremdunterbr);
            i->Fremd += 1;
            return FALSE;
        }

    } else if (!ntu_intr_erkannt(i)) {

        InterlockedIncrement((LONG *)&ntu_zaehler.Fremdunterbr);
        i->Fremd += 1;
        return FALSE;
    }

    ntu_intr_mask(i);
    i->Gesperrt = 1;
    i->Unt += 1;
    InterlockedIncrement((LONG *)&ntu_zaehler.Unterbrechungen);
    KeInsertQueueDpc(&i->Dpc, 0, 0);
    return TRUE;
}

VOID
ntu_dpc (PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
    ntu_intr *i = (ntu_intr *)ctx;

    KeSetEvent(&i->Wake, 0, FALSE);
}

VOID
ntu_ithread_body (PVOID ctx)
{
    ntu_intr *i = (ntu_intr *)ctx;

    for (;;) {

        KeWaitForSingleObject(&i->Wake, Executive, KernelMode, FALSE, NULL);

        if (i->Stop != 0) {
            break;
        }

        InterlockedIncrement((LONG *)&ntu_zaehler.Fadenlaeufe);
        i->Handler(i->Arg);          /* der unveraenderte FreeBSD-Handler */

        /*  xHCI: Anschlussaenderungen nachholen, falls der Baustein
         *  USBSTS.PCD nicht setzt -- ober/ntxhcipcd.c.  Auf PASSIVE_LEVEL
         *  und ohne gehaltene Sperre, so wie usb_needs_explore es will.  */

        if (i->ProgIf == PCIP_SERIALBUS_USB_XHCI) {
            ntu_xhci_pcd_nachholen(i->Arg, i->Tag, i->Handle, i->OpBase);
        }

        /*  ERST die Sperre loesen, DANN die Quelle freigeben.
         *
         *  Die erste Fassung tat es andersherum.  Faellt dann zwischen
         *  den beiden Zeilen ein neues Ereignis an, zieht der Baustein
         *  die Leitung, der ISR sieht Gesperrt = 1 und meldet "nicht
         *  meins".  Die Leitung ist pegelgesteuert, niemand nimmt sie
         *  ab, NT ruft sofort wieder -- und auf einem Prozessor kommt
         *  dieser Faden nie mehr dran, um Gesperrt zu loeschen.
         *
         *  Unter QEMU sichtbar geworden: Startbild stand minutenlang,
         *  der Herzschlag-Faden kam kein einziges Mal, und die Stichproben
         *  des Befehlszaehlers lagen im Eingangscode des KINTERRUPT-
         *  Objekts.  Im Lauf davor war schlicht kein Ereignis in das
         *  Fenster gefallen.
         *
         *  Auf x86 werden Speicherschreibzugriffe in Programmreihenfolge
         *  sichtbar, und WRITE_REGISTER_ULONG ist ein solcher; die
         *  Reihenfolge hier reicht also ohne eigene Schranke.  */

        /*  Nur freigeben, wenn der ISR wirklich gesperrt hat: der Faden
         *  wird auch vom Sicherungstakt geweckt (ntgeraet.c), und ein
         *  Freigeben ohne Sperren schriebe den nie gemerkten Wert zurueck.  */

        if (!i->Abfrage && i->Gesperrt != 0) {
            i->Gesperrt = 0;
            ntu_intr_unmask(i);
        }
    }
}


/* ====================================================================
 *  Nachgereichte Arbeit
 *
 *  callout_init_mtx heisst in FreeBSD: der Rueckruf laeuft MIT dem
 *  Mutex.  Das ist kein Nebensatz -- wer ihn uebersieht, laesst den
 *  Rueckruf ungeschuetzt laufen und sucht die Folgen spaeter im
 *  Uebertragungsring.
 *
 *  Ein DPC kann den Mutex nicht nehmen.  Also kommt auch der Rueckruf
 *  in einen Faden.
 * ==================================================================== */

#define NTU_WORK_RING  64

static struct callout *ntu_work[NTU_WORK_RING];
static ULONG           ntu_work_head;
static ULONG           ntu_work_tail;
static KSPIN_LOCK      ntu_work_lock;
static KEVENT          ntu_work_wake;
static ntu_thread      ntu_work_thread;
static ULONG           ntu_work_lost;

int
ntu_defer_callout (struct callout *c)
{
    KIRQL old;
    ULONG next;

    KeAcquireSpinLock(&ntu_work_lock, &old);

    next = (ntu_work_head + 1) % NTU_WORK_RING;

    if (next == ntu_work_tail) {

        /*  Ring voll.  Nicht ueberschreiben und nicht warten -- wir sind
         *  auf DISPATCH_LEVEL.  Gezaehlt wird, damit es im Bericht
         *  sichtbar wird statt spurlos zu verschwinden.  */

        ntu_work_lost += 1;
        InterlockedIncrement((LONG *)&ntu_zaehler.ArbeitVerloren);
        KeReleaseSpinLock(&ntu_work_lock, old);
        return 0;
    }

    ntu_work[ntu_work_head] = c;
    ntu_work_head = next;

    KeReleaseSpinLock(&ntu_work_lock, old);
    KeSetEvent(&ntu_work_wake, 0, FALSE);
    return 1;
}

static VOID
ntu_work_body (PVOID ctx)
{
    for (;;) {

        struct callout *c = 0;
        KIRQL           old;

        KeWaitForSingleObject(&ntu_work_wake, Executive, KernelMode,
                              FALSE, NULL);

        if (ntu_work_thread.Stop != 0) {
            break;
        }

        for (;;) {

            KeAcquireSpinLock(&ntu_work_lock, &old);

            if (ntu_work_tail == ntu_work_head) {
                KeReleaseSpinLock(&ntu_work_lock, old);
                break;
            }

            c = ntu_work[ntu_work_tail];
            ntu_work_tail = (ntu_work_tail + 1) % NTU_WORK_RING;

            KeReleaseSpinLock(&ntu_work_lock, old);

            if (c == 0 || c->Fn == 0) {
                continue;
            }

            c->Pending = 0;

            /*  Mit dem Mutex, wie FreeBSD es zusagt.  */

            if (c->Mtx != 0) {
                mtx_lock(c->Mtx);
                c->Fn(c->Arg);
                mtx_unlock(c->Mtx);
            } else {
                c->Fn(c->Arg);
            }
        }
    }
}

VOID
ntu_work_stop (VOID)
{
    if (ntu_work_thread.Handle == 0) {
        return;
    }

    ntu_work_thread.Stop = TRUE;
    KeSetEvent(&ntu_work_wake, 0, FALSE);
    ntu_thread_stop(&ntu_work_thread);
}

VOID
ntu_work_start (VOID)
{
    KeInitializeSpinLock(&ntu_work_lock);
    KeInitializeEvent(&ntu_work_wake, SynchronizationEvent, FALSE);
    ntu_work_head = 0;
    ntu_work_tail = 0;
    ntu_work_lost = 0;
    ntu_thread_start(&ntu_work_thread, ntu_work_body, 0, "usb work");
}

ULONG
ntu_work_verloren (VOID)
{
    return ntu_work_lost;
}
