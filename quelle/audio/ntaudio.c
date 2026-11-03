/*
 *  audio/ntaudio.c -- Rahmen fuer USBAUDIO.SYS (USB-Audio, UAC1).
 *
 *  Der eigentliche Treiber ist ober/ntuaudio.c.  Aufbau wie USBXBOX.SYS
 *  (xbox/ntxbox.c): eigenes Treiberobjekt, eigene IRP-Verteilung,
 *  Anmeldung des eigenen NTUDRV-Abschnitts bei usbnt.sys
 *  (ntu_treiber_anmelden); alle Stapelfunktionen per Import aus usbnt.sys.
 *
 *  Kein DriverUnload (wie USBXBOX): Entfernen = Dienst abschalten, neu
 *  starten.
 */

#include <ntddk.h>
#include <sys/module.h>
#include <nt/ntpaged.h>

PDRIVER_OBJECT NtuDriverObject;            /* ober/ntkbdmou.h: extern */
int            hz;                         /* sys/kernel.h: extern */

NTSTATUS ntua_ioctl (PDEVICE_OBJECT, PIRP);       /* ober/ntuaudio.c */
NTSTATUS ntua_aufraeumen (PDEVICE_OBJECT, PIRP);
NTSTATUS __stdcall DriverEntry (PDRIVER_OBJECT drv, PUNICODE_STRING regpfad);
#pragma alloc_text(INIT, DriverEntry)

/*  Importe aus usbnt.sys (nt/ntzusatz.c).  */
int __stdcall ntu_treiber_anmelden (const ntu_drvreg *r);
int __stdcall ntu_hz (void);
void          ntu_printf (const char *fmt, ...);

/*  Anfangs- und Endmarke des eigenen NTUDRV-Abschnitts, wie in
 *  nt/ntnewbus.c; die Eintraege legt DRIVER_MODULE in ntuaudio.c an.  */

#pragma section("NTUDRV$a", read)
#pragma section("NTUDRV$z", read)
__declspec(allocate("NTUDRV$a")) __attribute__((used))
static const ntu_drvreg * const audio_drv_start = 0;
__declspec(allocate("NTUDRV$z")) __attribute__((used))
static const ntu_drvreg * const audio_drv_end = 0;

static NTSTATUS
audio_erledigt (PDEVICE_OBJECT dev, PIRP irp)
{
    (void)dev;
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS
audio_nicht (PDEVICE_OBJECT dev, PIRP irp)
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
        drv->MajorFunction[i] = audio_nicht;
    }
    drv->MajorFunction[IRP_MJ_CREATE]                  = audio_erledigt;
    drv->MajorFunction[IRP_MJ_CLOSE]                   = audio_erledigt;
    drv->MajorFunction[IRP_MJ_CLEANUP]                 = ntua_aufraeumen;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL]          = ntua_ioctl;

    for (p = &audio_drv_start + 1; p < &audio_drv_end; p += 1) {
        if (*p != 0 && ntu_treiber_anmelden(*p) == 0) {
            n += 1;
        }
    }

    ntu_printf("usbaudio: Fassung %s, %d Treiber beim USB-Stapel angemeldet\n",
               NTU_DATEIFASSUNG, n);

    return (n > 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
