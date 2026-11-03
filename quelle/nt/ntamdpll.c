/*
 *  nt/ntamdpll.c -- AMD-PLL-Abhilfe fuer isochrone Uebertragungen.
 *
 *  Auf manchen AMD-Suedbruecken (SB700 bis Revision 0x3B, SB800, Hudson2,
 *  Bolton) spart die A-Link-Bruecke im Leerlauf Strom, indem sie eine
 *  PLL herunterfaehrt.  Waehrend eines isochronen Datenstroms -- Ton,
 *  Kamera -- fuehrt das zu Aussetzern.  Linux schaltet die Stromsparfunktion
 *  deshalb ab, solange ein isochroner Strom laeuft, und danach wieder ein.
 *
 *  Diese Datei ist Zeile fuer Zeile nach Linux geschrieben
 *  (drivers/usb/host/pci-quirks.c: amd_chipset_sb_type_init,
 *  usb_amd_find_chipset_info, usb_amd_quirk_pll).  Nicht aus dem
 *  Gedaechtnis, nicht "verbessert": die Registerfolge ist dort seit Jahren
 *  auf genau diesen Bruecken im Einsatz, und eigene Ideen haben an einer
 *  Stelle, die Chipsatzregister beschreibt, nichts verloren.
 *
 *  UNGEPRUEFT AUF ECHTER HARDWARE.  Unter QEMU gibt es keine
 *  AMD-Suedbruecke.  Nachgewiesen ist:
 *    - auf QEMU wird nichts erkannt und nichts angefasst;
 *    - im Trockenlauf (-DNTU_AMDPLL_TROCKEN -DNTU_AMDPLL_ERZWINGEN=700)
 *      ist die protokollierte Zugriffsfolge die von Linux.
 *
 *  Abschalten beim Bauen mit -DNTU_OHNE_AMDPLL.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include "ntunterlage.h"
#include <dev/pci/pcivar.h>

extern ntu_pci_ivars ntu_hc[];
extern ULONG         ntu_hc_count;

/* ---- aus pci-quirks.c ---------------------------------------------- */

#define AB_REG_BAR_LOW      0xe0
#define AB_REG_BAR_HIGH     0xe1
#define AB_REG_BAR_SB700    0xf0
#define AB_INDX(a)          ((a) + 0x00)
#define AB_DATA(a)          ((a) + 0x04)
#define AX_INDXC            0x30
#define AX_DATAC            0x34

#define NB_PCIE_INDX_ADDR   0xe0
#define NB_PCIE_INDX_DATA   0xe4
#define PCIE_P_CNTL         0x10040
#define BIF_NB              0x10002
#define NB_PIF0_PWRDOWN_0   0x01100012
#define NB_PIF0_PWRDOWN_1   0x01100013

enum { AMD_UNBEKANNT, AMD_SB600, AMD_SB700, AMD_SB800, AMD_HUDSON2,
       AMD_BOLTON, AMD_YANGTZE, AMD_TAISHAN, KEIN_AMD };

static const char *amd_namen[] = {
    "unbekannt", "SB600", "SB700", "SB800", "Hudson2", "Bolton",
    "Yangtze", "Taishan", "kein AMD"
};

static struct {
    int         geprueft;
    int         gen;
    UCHAR       rev;
    int         noetig;
    ULONG       sm_slot;         /* PCI_SLOT_NUMBER.u.AsULONG, Bus 0 */
    int         nb_da;
    ULONG       nb_slot;
    int         nb_type;
    LONG        isoc_reqs;
    KSPIN_LOCK  sperre;
} amd;

ULONG ntu_amdpll_aus;            /* wie oft abgeschaltet -- Herzschlag */


/* ====================================================================
 *  Zugriffe -- im Trockenlauf nur protokolliert
 * ==================================================================== */

#ifdef NTU_AMDPLL_TROCKEN

static ULONG trocken_cfg(ULONG slot, ULONG off)
{
    ntu_printf("amdpll: cfg lesen   slot %08x off %02x\n", slot, off);
    return (off == AB_REG_BAR_SB700) ? 0x00000b00 : 0x12345678;
}
#define CFG_R32(slot,off)       trocken_cfg((slot),(off))
#define CFG_W32(slot,off,v)     ntu_printf("amdpll: cfg schreib slot %08x off %02x = %08x\n", (slot), (off), (ULONG)(v))
#define IO_W8(port,v)           ntu_printf("amdpll: outb %02x -> %04x\n", (ULONG)(v), (ULONG)(port))
#define IO_R8(port)             (ntu_printf("amdpll: inb  %04x\n", (ULONG)(port)), (UCHAR)0x0b)
#define IO_W32(port,v)          ntu_printf("amdpll: outl %08x -> %04x\n", (ULONG)(v), (ULONG)(port))
#define IO_R32(port)            (ntu_printf("amdpll: inl  %04x\n", (ULONG)(port)), (ULONG)0x00000208)

#else

static ULONG
cfg_r32 (ULONG slot, ULONG off)
{
    ULONG v = 0;
    HalGetBusDataByOffset(PCIConfiguration, 0, slot, &v, off, 4);
    return v;
}

static VOID
cfg_w32 (ULONG slot, ULONG off, ULONG v)
{
    HalSetBusDataByOffset(PCIConfiguration, 0, slot, &v, off, 4);
}

#define CFG_R32(slot,off)       cfg_r32((slot),(off))
#define CFG_W32(slot,off,v)     cfg_w32((slot),(off),(v))
#define IO_W8(port,v)           WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)(port), (UCHAR)(v))
#define IO_R8(port)             READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)(port))
#define IO_W32(port,v)          WRITE_PORT_ULONG((PULONG)(ULONG_PTR)(port), (ULONG)(v))
#define IO_R32(port)            READ_PORT_ULONG((PULONG)(ULONG_PTR)(port))

#endif


/* ====================================================================
 *  Erkennung -- amd_chipset_sb_type_init + usb_amd_find_chipset_info
 *
 *  Linux sucht mit pci_get_device auf allen Bussen.  Die Bruecke und die
 *  Northbridge sitzen auf diesen Brettern immer auf Bus 0; mehr als Bus 0
 *  abzusuchen wuerde nur die Stelle verlaengern, an der etwas schiefgehen
 *  kann.
 * ==================================================================== */

static int
finde (USHORT ven, USHORT dev, ULONG *slot_aus, UCHAR *rev_aus)
{
    ULONG d, f;

    for (d = 0; d < 32; d += 1) {
        for (f = 0; f < 8; f += 1) {

            PCI_SLOT_NUMBER s;
            ULONG           id = 0xFFFFFFFFu;
            UCHAR           rev = 0;

            s.u.AsULONG = 0;
            s.u.bits.DeviceNumber   = d;
            s.u.bits.FunctionNumber = f;

            if (HalGetBusDataByOffset(PCIConfiguration, 0, s.u.AsULONG,
                                      &id, 0, 4) < 4) {
                continue;
            }

            if (id == ((ULONG)dev << 16 | ven)) {
                HalGetBusDataByOffset(PCIConfiguration, 0, s.u.AsULONG,
                                      &rev, 8, 1);
                *slot_aus = s.u.AsULONG;
                if (rev_aus != 0) {
                    *rev_aus = rev;
                }
                return 1;
            }
        }
    }

    return 0;
}

VOID
ntu_amdpll_erkennen (VOID)
{
    ULONG slot;
    UCHAR rev = 0;

    KeInitializeSpinLock(&amd.sperre);
    amd.geprueft = 1;
    amd.gen      = KEIN_AMD;

#ifdef NTU_OHNE_AMDPLL
    ntu_printf("amdpll: beim Bauen abgeschaltet\n");
    return;
#endif

#if defined(NTU_AMDPLL_ERZWINGEN)
    /*  Nur fuer den Trockenlauf unter QEMU: eine SB700 Rev 0x3A mit RS780
     *  vortaeuschen, damit die Zugriffsfolge protokolliert werden kann.  */
    amd.gen = (NTU_AMDPLL_ERZWINGEN == 800) ? AMD_SB800 : AMD_SB700;
    amd.rev = (NTU_AMDPLL_ERZWINGEN == 800) ? 0x41 : 0x3A;
    amd.sm_slot = 0x000000a0;       /* 14.0 */
    amd.nb_da = 1; amd.nb_slot = 0; amd.nb_type = 3;
    amd.noetig = 1;
    ntu_printf("amdpll: ERZWUNGEN %s Rev %02x, NB-Typ %d (Trockenlauf)\n",
               amd_namen[amd.gen], amd.rev, amd.nb_type);
    return;
#endif

    /* ---- amd_chipset_sb_type_init ---- */

    if (finde(0x1002, 0x4385, &slot, &rev)) {           /* SBX00 SMBus */
        if      (rev >= 0x10 && rev <= 0x1f) amd.gen = AMD_SB600;
        else if (rev >= 0x30 && rev <= 0x3f) amd.gen = AMD_SB700;
        else if (rev >= 0x40 && rev <= 0x4f) amd.gen = AMD_SB800;
        else                                 amd.gen = AMD_UNBEKANNT;
    } else if (finde(0x1022, 0x780b, &slot, &rev)) {    /* Hudson2 SMBus */
        if      (rev >= 0x11 && rev <= 0x14) amd.gen = AMD_HUDSON2;
        else if (rev >= 0x15 && rev <= 0x18) amd.gen = AMD_BOLTON;
        else if (rev >= 0x39 && rev <= 0x3a) amd.gen = AMD_YANGTZE;
        else                                 amd.gen = AMD_UNBEKANNT;
    } else if (finde(0x1022, 0x145c, &slot, &rev)) {
        amd.gen = AMD_TAISHAN;
    } else {
        ntu_printf("amdpll: keine AMD-Suedbruecke -- nicht noetig\n");
        return;
    }

    amd.rev     = rev;
    amd.sm_slot = slot;

    /* ---- usb_amd_find_chipset_info ---- */

    switch (amd.gen) {
        case AMD_SB700:   amd.noetig = (rev <= 0x3B); break;
        case AMD_SB800:
        case AMD_HUDSON2:
        case AMD_BOLTON:  amd.noetig = 1;             break;
        default:          amd.noetig = 0;             break;
    }

    if (!amd.noetig) {
        ntu_printf("amdpll: %s Rev %02x -- nicht noetig\n",
                   amd_namen[amd.gen], rev);
        return;
    }

    if      (finde(0x1022, 0x9601, &amd.nb_slot, 0)) { amd.nb_da = 1; amd.nb_type = 1; }
    else if (finde(0x1022, 0x1510, &amd.nb_slot, 0)) { amd.nb_da = 1; amd.nb_type = 2; }
    else if (finde(0x1022, 0x9600, &amd.nb_slot, 0)) { amd.nb_da = 1; amd.nb_type = 3; }

    ntu_printf("amdpll: %s Rev %02x -- Abhilfe AKTIV, Northbridge %s\n",
               amd_namen[amd.gen], rev,
               amd.nb_da ? (amd.nb_type == 1 ? "RS880" :
                            amd.nb_type == 2 ? "Fam14h" : "RS780")
                         : "keine bekannte");
}


/* ====================================================================
 *  Umschalten -- usb_amd_quirk_pll
 * ==================================================================== */

static VOID
amd_pll (int disable)
{
    ULONG addr, addr_low, addr_high, val;
    ULONG bit = disable ? 0 : 1;
    KIRQL alt;

    if (!amd.noetig) {
        return;
    }

    KeAcquireSpinLock(&amd.sperre, &alt);

    if (disable) {
        amd.isoc_reqs += 1;
        if (amd.isoc_reqs > 1) {
            KeReleaseSpinLock(&amd.sperre, alt);
            return;
        }
        ntu_amdpll_aus += 1;
    } else {
        amd.isoc_reqs -= 1;
        if (amd.isoc_reqs > 0) {
            KeReleaseSpinLock(&amd.sperre, alt);
            return;
        }
    }

    if (amd.gen == AMD_SB800 || amd.gen == AMD_HUDSON2 ||
        amd.gen == AMD_BOLTON) {
        IO_W8(0xcd6, AB_REG_BAR_LOW);
        addr_low = IO_R8(0xcd7);
        IO_W8(0xcd6, AB_REG_BAR_HIGH);
        addr_high = IO_R8(0xcd7);
        addr = addr_high << 8 | addr_low;

        IO_W32(AB_INDX(addr), 0x30);
        IO_W32(AB_DATA(addr), 0x40);
        IO_W32(AB_INDX(addr), 0x34);
        val = IO_R32(AB_DATA(addr));
    } else if (amd.gen == AMD_SB700 && amd.rev <= 0x3b) {
        addr = CFG_R32(amd.sm_slot, AB_REG_BAR_SB700);
        IO_W32(AB_INDX(addr), AX_INDXC);
        IO_W32(AB_DATA(addr), 0x40);
        IO_W32(AB_INDX(addr), AX_DATAC);
        val = IO_R32(AB_DATA(addr));
    } else {
        KeReleaseSpinLock(&amd.sperre, alt);
        return;
    }

    if (disable) {
        val &= ~0x08;
        val |= (1 << 4) | (1 << 9);
    } else {
        val |= 0x08;
        val &= ~((1 << 4) | (1 << 9));
    }
    IO_W32(AB_DATA(addr), val);

    if (!amd.nb_da) {
        KeReleaseSpinLock(&amd.sperre, alt);
        return;
    }

    if (amd.nb_type == 1 || amd.nb_type == 3) {
        addr = PCIE_P_CNTL;
        CFG_W32(amd.nb_slot, NB_PCIE_INDX_ADDR, addr);
        val = CFG_R32(amd.nb_slot, NB_PCIE_INDX_DATA);

        val &= ~(1 | (1 << 3) | (1 << 4) | (1 << 9) | (1 << 12));
        val |= bit | (bit << 3) | (bit << 12);
        val |= ((!bit) << 4) | ((!bit) << 9);
        CFG_W32(amd.nb_slot, NB_PCIE_INDX_DATA, val);

        addr = BIF_NB;
        CFG_W32(amd.nb_slot, NB_PCIE_INDX_ADDR, addr);
        val = CFG_R32(amd.nb_slot, NB_PCIE_INDX_DATA);
        val &= ~(1 << 8);
        val |= bit << 8;
        CFG_W32(amd.nb_slot, NB_PCIE_INDX_DATA, val);
    } else if (amd.nb_type == 2) {
        addr = NB_PIF0_PWRDOWN_0;
        CFG_W32(amd.nb_slot, NB_PCIE_INDX_ADDR, addr);
        val = CFG_R32(amd.nb_slot, NB_PCIE_INDX_DATA);
        if (disable) val &= ~(0x3f << 7); else val |= 0x3f << 7;
        CFG_W32(amd.nb_slot, NB_PCIE_INDX_DATA, val);

        addr = NB_PIF0_PWRDOWN_1;
        CFG_W32(amd.nb_slot, NB_PCIE_INDX_ADDR, addr);
        val = CFG_R32(amd.nb_slot, NB_PCIE_INDX_DATA);
        if (disable) val &= ~(0x3f << 7); else val |= 0x3f << 7;
        CFG_W32(amd.nb_slot, NB_PCIE_INDX_DATA, val);
    }

    KeReleaseSpinLock(&amd.sperre, alt);
}


/* ====================================================================
 *  Fuer die Oberkante
 *
 *  Nur fuer Hostbausteine auf der AMD-Bruecke selbst -- Hersteller 1002
 *  oder 1022.  Eine NEC-Steckkarte auf demselben Brett ist nicht
 *  betroffen; Linux setzt die Abhilfe genauso nur bei AMD-Bausteinen
 *  (ohci-pci, ehci-pci, xhci-pci jeweils mit Herstellerpruefung).
 * ==================================================================== */

int
ntu_amdpll_betrifft (void *hc)
{
    ULONG i;

    if (!amd.noetig) {
        return 0;
    }

#if defined(NTU_AMDPLL_ERZWINGEN)
    (void)hc;
    return 1;
#else
    for (i = 0; i < ntu_hc_count; i += 1) {
        extern device_t ntu_hc_dev[];

        if ((void *)ntu_hc_dev[i] == hc) {
            return ntu_hc[i].Vendor == 0x1002 || ntu_hc[i].Vendor == 0x1022;
        }
    }

    return 0;
#endif
}

VOID ntu_amdpll_isoc_an  (VOID) { amd_pll(1); }   /* PLL-Sparen AUS */
VOID ntu_amdpll_isoc_aus (VOID) { amd_pll(0); }   /* PLL-Sparen wieder AN */
