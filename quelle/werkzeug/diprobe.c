/*
 *  werkzeug/diprobe.c -- welche Joysticks sieht DirectInput?
 *
 *      diprobe        DirectInput 3 (die Fassung auf NT 4.0) nach Joysticks
 *                     fragen, je Geraet Achsen, Knoepfe und Hutschalter;
 *                     dazu winmm: joyGetDevCaps und das gemeldete szRegKey
 *
 *  Gegenprobe zu joytest: ein Controller kann in winmm erscheinen und in
 *  DirectInput trotzdem fehlen.  DirectInput 3 nimmt einen winmm-Joystick
 *  nur auf, wenn unter
 *      MediaResources\Joystick\<szRegKey>\CurrentJoystickSettings
 *  ein Joystick<n>OEMName steht und dieser OEM-Typ existiert.
 *
 *  Strukturen in der DX3-Groesse, damit es auch mit DINPUT.DLL von NT 4.0
 *  laeuft (die neueren Groessen weist es ab).  kernel32, winmm, dinput.
 */

#include <windows.h>
#include "ntbasis.h"
#include <mmsystem.h>

typedef struct {                         /* DIDEVICEINSTANCE_DX3A */
    DWORD dwSize;
    GUID  guidInstance;
    GUID  guidProduct;
    DWORD dwDevType;
    char  tszInstanceName[MAX_PATH];
    char  tszProductName[MAX_PATH];
} GERAET_DX3;

typedef struct {                         /* DIDEVCAPS_DX3 */
    DWORD dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs;
} FAEHIG_DX3;

/*  Nur die benutzten Methoden; Reihenfolge wie in dinput.h.  */
typedef struct DI_vt  DI_vt;
typedef struct DID_vt DID_vt;
typedef struct { DI_vt  *v; } DI;
typedef struct { DID_vt *v; } DID;
typedef BOOL (WINAPI *AUFZAEHLEN)(const GERAET_DX3 *, void *);

struct DI_vt {
    HRESULT (WINAPI *QueryInterface)(DI *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(DI *);
    ULONG   (WINAPI *Release)(DI *);
    HRESULT (WINAPI *CreateDevice)(DI *, const GUID *, DID **, void *);
    HRESULT (WINAPI *EnumDevices)(DI *, DWORD, AUFZAEHLEN, void *, DWORD);
};
struct DID_vt {
    HRESULT (WINAPI *QueryInterface)(DID *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(DID *);
    ULONG   (WINAPI *Release)(DID *);
    HRESULT (WINAPI *GetCapabilities)(DID *, FAEHIG_DX3 *);
};

#define DIDEVTYPE_JOYSTICK  4
#define DIEDFL_ALLDEVICES   0

HRESULT WINAPI DirectInputCreateA(HINSTANCE, DWORD, DI **, void *);

static DI *di;
static int gefunden;

static BOOL WINAPI
je_geraet (const GERAET_DX3 *g, void *kontext)
{
    DID        *d;
    FAEHIG_DX3  f;
    HRESULT     r;

    (void)kontext;
    gefunden += 1;
    printf("  DirectInput: \"%s\"", g->tszInstanceName);
    printf(" (Produkt \"%s\")\n", g->tszProductName);

    r = di->v->CreateDevice(di, &g->guidInstance, &d, NULL);
    if (r != 0) {
        printf("    CreateDevice: Fehler %08lx\n", (unsigned long)r);
        return TRUE;
    }
    memset(&f, 0, sizeof(f));
    f.dwSize = sizeof(f);
    r = d->v->GetCapabilities(d, &f);
    if (r == 0) {
        printf("    %lu Achsen, ", f.dwAxes);
        printf("%lu Knoepfe, ", f.dwButtons);
        printf("%lu Hutschalter\n", f.dwPOVs);
    } else {
        printf("    GetCapabilities: Fehler %08lx\n", (unsigned long)r);
    }
    d->v->Release(d);
    return TRUE;
}

int
main (void)
{
    JOYCAPSA c;
    UINT     n, k;
    HRESULT  r;

    n = joyGetNumDevs();
    printf("winmm: joyGetNumDevs = %u\n", n);
    for (k = 0; k < n && k < 2; k += 1) {
        memset(&c, 0, sizeof(c));
        if (joyGetDevCapsA(k, &c, sizeof(c)) == JOYERR_NOERROR && c.szPname[0]) {
            printf("  Joystick %u: \"%s\"", k + 1, c.szPname);
            printf(", %u Knoepfe, szRegKey ", c.wNumButtons);
            printf("\"%s\"\n", c.szRegKey);
        } else {
            printf("  Joystick %u: nicht angeschlossen\n", k + 1);
        }
    }

    r = DirectInputCreateA(GetModuleHandleA(NULL), 0x0300, &di, NULL);
    if (r != 0) {
        printf("DirectInputCreateA: Fehler %08lx\n", (unsigned long)r);
        return 2;
    }
    di->v->EnumDevices(di, DIDEVTYPE_JOYSTICK, je_geraet, NULL, DIEDFL_ALLDEVICES);
    printf("DirectInput: %d Joystick(s)\n", gefunden);
    di->v->Release(di);
    return gefunden ? 0 : 1;
}
