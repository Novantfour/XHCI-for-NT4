/*
 *  xbox/ntxbox.c -- Rahmen fuer USBXBOX.SYS (Gamepads und Joysticks).
 *
 *  Der eigentliche Treiber ist ober/ntujoy.c, unveraendert: Xbox 360,
 *  Xbox One/Series (GIP) und HID-Gamepads.  Frueher war er in usbnt.sys
 *  eingebaut; jetzt ist er ein eigener Kerntreiber mit eigenem Dienst.
 *  usbnt.sys enthaelt ihn nicht mehr.
 *
 *  Was dieser Rahmen stellt -- das, was ntujoy.c bisher vom Umfeld in
 *  usbnt.sys bekam:
 *    NtuDriverObject   das eigene Treiberobjekt; ntujoy legt daran
 *                      \Device\UsbJoyN an, die IRPs kommen also hierher
 *    hz                Takt des Stapels, beim Start aus usbnt.sys geholt
 *    IRP-Verteilung    Steueraufrufe an ntuj_ioctl, Oeffnen/Schliessen ok
 *  und die Anmeldung: DRIVER_MODULE in ntujoy.c legt einen Eintrag im
 *  Abschnitt NTUDRV dieses Abbilds an (wie in usbnt.sys).  Hier wird
 *  dieser Abschnitt abgelaufen und jeder Eintrag bei usbnt.sys
 *  angemeldet; der Stapel zaehlt danach betroffene Geraete neu auf
 *  (nt/ntzusatz.c).
 *
 *  Alle Stapelfunktionen kommen per Import aus usbnt.sys.  Laedt
 *  usbnt.sys nicht, laedt dieser Treiber auch nicht (der Lader findet
 *  die Importe nicht) -- gewollt: ohne Stapel hat er nichts zu tun.
 *
 *  Kein DriverUnload: ein angemeldeter Treiber laesst sich aus dem
 *  Stapel nicht wieder austragen, ohne dass Geraete an ihm haengen.
 *  Entfernen = Dienst abschalten und neu starten (XBOXINST aus).
 */

#include <ntddk.h>
#include <sys/module.h>
#include <nt/ntpaged.h>

PDRIVER_OBJECT NtuDriverObject;            /* ober/ntkbdmou.h: extern */
int            hz;                         /* sys/kernel.h: extern */

NTSTATUS ntuj_ioctl (PDEVICE_OBJECT, PIRP);  /* ober/ntujoy.c */
NTSTATUS __stdcall DriverEntry (PDRIVER_OBJECT drv, PUNICODE_STRING regpfad);
#pragma alloc_text(INIT, DriverEntry)

/*  Importe aus usbnt.sys (nt/ntzusatz.c).  */
int __stdcall ntu_treiber_anmelden (const ntu_drvreg *r);
int __stdcall ntu_hz (void);
void          ntu_printf (const char *fmt, ...);

/*  Anfangs- und Endmarke des eigenen NTUDRV-Abschnitts, wie in
 *  nt/ntnewbus.c; die Eintraege legt DRIVER_MODULE in ntujoy.c an.  */

#pragma section("NTUDRV$a", read)
#pragma section("NTUDRV$z", read)
__declspec(allocate("NTUDRV$a")) __attribute__((used))
static const ntu_drvreg * const xbox_drv_start = 0;
__declspec(allocate("NTUDRV$z")) __attribute__((used))
static const ntu_drvreg * const xbox_drv_end = 0;

static NTSTATUS
xbox_erledigt (PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
xbox_nicht (PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    irp->IoStatus.Status      = STATUS_INVALID_DEVICE_REQUEST;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS __stdcall
DriverEntry (PDRIVER_OBJECT drv, PUNICODE_STRING regpfad)
{
    const ntu_drvreg * const *p;
    NTU_PAGED_CODE();
    ULONG i;
    int   n = 0;

    (void)regpfad;

    NtuDriverObject = drv;
    hz = ntu_hz();

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i += 1) {
        drv->MajorFunction[i] = xbox_nicht;
    }
    drv->MajorFunction[IRP_MJ_CREATE]                  = xbox_erledigt;
    drv->MajorFunction[IRP_MJ_CLOSE]                   = xbox_erledigt;
    drv->MajorFunction[IRP_MJ_CLEANUP]                 = xbox_erledigt;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL]          = ntuj_ioctl;
    drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = ntuj_ioctl;

    for (p = &xbox_drv_start + 1; p < &xbox_drv_end; p += 1) {
        if (*p != 0 && ntu_treiber_anmelden(*p) == 0) {
            n += 1;
        }
    }

    ntu_printf("usbxbox: Fassung %s, %d Treiber beim USB-Stapel angemeldet\n",
               NTU_DATEIFASSUNG, n);

    return (n > 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
