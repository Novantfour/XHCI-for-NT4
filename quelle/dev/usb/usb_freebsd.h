/*
 *  dev/usb/usb_freebsd.h -- die eigene Fassung.
 *
 *  Das ist der sauberste Eingriff der ganzen Unterlage: FreeBSD haelt
 *  seine Wahlschalter in genau dieser Datei, und weil das Verzeichnis
 *  der Unterlage im Suchpfad vor dem Quellbaum steht, reicht es, sie zu
 *  ersetzen.  Keine Zeile Treiberquelltext wird angefasst.
 *
 *  Abgeschaltet wird, was unter NT kein Gegenstueck hat:
 *
 *      UGEN, USER_IO      /dev/ugen.  Die Oberkante ist ein SCSI-
 *                         Miniport und zwei Klassenfilter, kein
 *                         Zeichengeraet.
 *      DEVCTL             devd-Meldungen.  Niemand hoert zu.
 *      COMPAT_LINUX       Linux-Nachbildung in FreeBSD.  Doppelt
 *                         uebersetzt waere absurd.
 *      MBUF               nur fuer usb_pf, das Mitschneiden.
 *      PF                 dito.
 *      ROOT_MOUNT_HOLD    haengt am FreeBSD-Bootablauf.
 *      ID_SECTION         linker set fuer Geraetekennungen.
 *
 *  Angelassen wird, was Arbeit leistet: BUSDMA, TT_SUPPORT (ohne das
 *  laufen langsame Geraete an einem EHCI-Hub nicht), MSCTEST,
 *  PER_BUS_PROCESS, DISABLE_ENUM.
 *
 *  MALLOC_WAITOK bleibt an -- ExAllocatePoolWithTag schlaeft zwar nie,
 *  aber der Stapel prueft jede Rueckgabe ohnehin.
 */
#ifndef _USB_FREEBSD_H_
#define _USB_FREEBSD_H_

#define USB_HAVE_UGEN                  0
#define USB_HAVE_DEVCTL                0
#define USB_HAVE_BUSDMA                1
#define USB_HAVE_COMPAT_LINUX          0
#define USB_HAVE_USER_IO               0
#define USB_HAVE_MBUF                  0
#define USB_HAVE_TT_SUPPORT            1
#define USB_HAVE_POWERD                0
#define USB_HAVE_MSCTEST               1
#define USB_HAVE_MSCTEST_AUTOQUIRK     0
#define USB_HAVE_MSCTEST_DETACH        1
#define USB_HAVE_PF                    0
#define USB_HAVE_ROOT_MOUNT_HOLD       0
#define USB_HAVE_ID_SECTION            0
#define USB_HAVE_PER_BUS_PROCESS       1
#define USB_HAVE_FIXED_ENDPOINT        0
#define USB_HAVE_FIXED_IFACE           0
#define USB_HAVE_FIXED_CONFIG          0
#define USB_HAVE_FIXED_PORT            0
#define USB_HAVE_DISABLE_ENUM          1
#define USB_HAVE_MALLOC_WAITOK         1

#define USB_TD_GET_PROC(td)   ((td)->td_proc)
#define USB_PROC_GET_GID(td)  (0)

#define USB_HOST_ALIGN     32     /* Pufferausrichtung, xHCI-vertraeglich */
#define USB_FS_ISOC_UFRAME_MAX 4
#define USB_BUS_MAX        256
#define USB_MAX_DEVICES    128
#define USB_CONFIG_MAX     65535
#define USB_IFACE_MAX      32
#define USB_FIFO_MAX       128
#define USB_MAX_EP_STREAMS 8
#define USB_MAX_EP_UNITS   32
#define USB_MAX_PORTS      255
#define USB_MAX_FS_ISOC_FRAMES_PER_XFER (120)
#define USB_MAX_HS_ISOC_FRAMES_PER_XFER (8*120)
#define USB_HUB_MAX_DEPTH  5
#define USB_EP0_BUFSIZE    1024
#define USB_CS_RESET_LIMIT 20
#define USB_MAX_AUTO_QUIRK 4
#define USB_CALLOUT_ZERO_TICKS 1

/*  Abfragebetrieb gibt es hier nicht: der Stapel laeuft immer mit
 *  Unterbrechungen.  Der Kerndebugger von NT haelt den Rechner an, ohne
 *  den Treiber zu fragen -- ein Abfragepfad haette nichts, wovon er
 *  gerufen wuerde.  */
#define USB_IN_POLLING_MODE_FUNC()  (0)
#define USB_IN_POLLING_MODE_VALUE() (0)

typedef unsigned int  usb_timeout_t;
typedef unsigned int  usb_frlength_t;
typedef unsigned int  usb_frcount_t;
typedef unsigned int  usb_size_t;
typedef unsigned int  usb_ticks_t;
typedef unsigned short usb_power_mask_t;
typedef unsigned short usb_stream_t;
#endif
