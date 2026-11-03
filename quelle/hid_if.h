/*
 *  hid_if.h -- von Hand statt aus hid_if.m (kobj).
 *
 *  FreeBSD erzeugt diese Datei aus sys/dev/hid/hid_if.m.  Die Methoden
 *  gehoeren zum HID-Bus (hidbus), den die Unterlage NICHT nachbildet:
 *  die NT-Oberkanten (ober/ntums.c, ober/ntujoy.c) sitzen direkt auf dem
 *  USB-Stapel und benutzen aus dev/hid/hid.c nur den Deskriptor-Parser
 *  (hid_start_parse, hid_get_item, hid_locate, hid_get_data, ...).
 *
 *  Damit hid.c unveraendert uebersetzt und bindet, gibt es jede Methode
 *  -- und jede antwortet "nicht vorhanden".  Aufgerufen wird keine.
 */

#ifndef _NTU_HID_IF_H_
#define _NTU_HID_IF_H_

#include <sys/errno.h>

struct hid_rdesc_info;

static __inline void HID_INTR_SETUP(device_t dev, device_t child, hid_intr_t intr,
    void *context, struct hid_rdesc_info *rdesc) { }
static __inline void HID_INTR_UNSETUP(device_t dev, device_t child) { }
static __inline int  HID_INTR_START(device_t dev, device_t child) { return (ENXIO); }
static __inline int  HID_INTR_STOP(device_t dev, device_t child) { return (ENXIO); }
static __inline void HID_INTR_POLL(device_t dev, device_t child) { }
static __inline int  HID_GET_RDESC(device_t dev, device_t child, void *data,
    hid_size_t len) { return (ENXIO); }
static __inline int  HID_READ(device_t dev, device_t child, void *data,
    hid_size_t maxlen, hid_size_t *actlen) { return (ENXIO); }
static __inline int  HID_WRITE(device_t dev, device_t child, const void *data,
    hid_size_t len) { return (ENXIO); }
static __inline int  HID_GET_REPORT(device_t dev, device_t child, void *data,
    hid_size_t maxlen, hid_size_t *actlen, uint8_t type, uint8_t id) { return (ENXIO); }
static __inline int  HID_SET_REPORT(device_t dev, device_t child, const void *data,
    hid_size_t len, uint8_t type, uint8_t id) { return (ENXIO); }
static __inline int  HID_SET_IDLE(device_t dev, device_t child, uint16_t duration,
    uint8_t id) { return (ENXIO); }
static __inline int  HID_SET_PROTOCOL(device_t dev, device_t child,
    uint16_t protocol) { return (ENXIO); }
static __inline int  HID_IOCTL(device_t dev, device_t child, unsigned long cmd,
    uintptr_t data) { return (ENXIO); }

#endif
