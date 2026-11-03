/*
 *  nt/ntsysctl.c -- FreeBSD-Schalter (SYSCTL_INT) per Registry setzen.
 *
 *      Services\<dienst>\Parameters\Sysctl   REG_SZ
 *          "hw.usb.xhci.ctlstep=1;hw.usb.xhci.polling=1"
 *
 *  SYSCTL_INT (sys/sysctl.h) legt jeden Schalter als "Eltern, Name,
 *  Adresse" in die Binder-Sektion NTUSYS$m; der Binder legt NTUSYS$a, $m
 *  und $z alphabetisch hintereinander, dazwischen liegen alle Eintraege.
 *  Der Name entsteht wie bei FreeBSD: "_hw_usb_xhci" + "ctlstep" ->
 *  "hw.usb.xhci.ctlstep".
 *
 *  Gelesen in DriverEntry, bevor ein Controller angehaengt wird.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include "ntunterlage.h"

#pragma section("NTUSYS$a", read)
#pragma section("NTUSYS$z", read)

__declspec(allocate("NTUSYS$a")) __attribute__((used))
static const ntu_sysctlreg * const ntu_sysctl_anfang = 0;
__declspec(allocate("NTUSYS$z")) __attribute__((used))
static const ntu_sysctlreg * const ntu_sysctl_ende = 0;

/*  "_hw_usb_xhci" + "ctlstep" -> "hw.usb.xhci.ctlstep" und vergleichen.  */

static int
ntu_sysctl_gleich (const ntu_sysctlreg *r, const char *voll, int n)
{
    char        name[96];
    const char *e = r->eltern;
    int         k = 0;

    if (*e == '_') e += 1;
    for (; *e != '\0' && k < 80; e += 1) {
        name[k++] = (*e == '_') ? '.' : *e;
    }
    name[k++] = '.';
    for (e = r->name; *e != '\0' && k < 95; e += 1) {
        name[k++] = *e;
    }
    name[k] = '\0';

    if (k != n) {
        return 0;
    }
    for (k = 0; k < n; k += 1) {
        if (name[k] != voll[k]) {
            return 0;
        }
    }
    return 1;
}

static int
ntu_sysctl_setzen (const char *voll, int n, int wert)
{
    const ntu_sysctlreg * const *p;

    for (p = &ntu_sysctl_anfang + 1; p < &ntu_sysctl_ende; p += 1) {
        if (*p != 0 && (*p)->wert != 0 && ntu_sysctl_gleich(*p, voll, n)) {
            *(*p)->wert = wert;
            return 1;
        }
    }
    return 0;
}

VOID
ntu_sysctl_lesen (PUNICODE_STRING reg)
{
    WCHAR                    pfad[260], puffer[256];
    UNICODE_STRING           u;
    RTL_QUERY_REGISTRY_TABLE t[2];
    ULONG                    n = reg->Length / sizeof(WCHAR);
    char                     a[256];
    int                      i, k;

    if (n >= 240) {
        return;
    }
    memcpy(pfad, reg->Buffer, n * sizeof(WCHAR));
    memcpy(pfad + n, L"\\Parameters", 12 * sizeof(WCHAR));
    pfad[n + 11] = 0;

    u.Buffer        = puffer;
    u.Length        = 0;
    u.MaximumLength = sizeof(puffer) - sizeof(WCHAR);

    memset(t, 0, sizeof(t));
    t[0].Flags        = RTL_QUERY_REGISTRY_DIRECT;
    t[0].Name         = L"Sysctl";
    t[0].EntryContext = &u;

    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, pfad, t, 0, 0)) ||
        u.Length == 0) {
        return;
    }

    for (k = 0; k < (int)(u.Length / sizeof(WCHAR)) && k < 255; k += 1) {
        a[k] = (char)u.Buffer[k];
    }
    a[k] = '\0';

    /*  "name=wert;name=wert" (auch Leerzeichen oder Komma als Trenner).  */

    for (i = 0; a[i] != '\0'; ) {
        int s = i, gl = -1, e, wert = 0, neg = 0, j;

        while (a[i] != '\0' && a[i] != ';' && a[i] != ',' && a[i] != ' ') {
            if (a[i] == '=' && gl < 0) gl = i;
            i += 1;
        }
        e = i;
        if (a[i] != '\0') i += 1;
        if (gl <= s || gl >= e - 1) continue;

        j = gl + 1;
        if (a[j] == '-') { neg = 1; j += 1; }
        for (; j < e && a[j] >= '0' && a[j] <= '9'; j += 1) {
            wert = wert * 10 + (a[j] - '0');
        }
        if (neg) wert = -wert;

        a[gl] = '\0';
        ntu_printf("sysctl: %s = %d%s\n", a + s, wert,
                   ntu_sysctl_setzen(a + s, gl - s, wert) ? "" : "  (unbekannt, uebergangen)");
        a[gl] = '=';
    }
}
