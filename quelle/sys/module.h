/*  sys/module.h -- Treiberanmeldung ueber einen Abschnitt, nicht ueber
 *  einen Ladepfad.
 *
 *  FreeBSD sammelt seine DRIVER_MODULE-Eintraege in einem linker set.
 *  Das gleiche Mittel gibt es unter dem MSVC-Binder auch: benannte
 *  Abschnitte mit $-Nachsilbe werden alphabetisch zusammengelegt.  Also
 *      NTUDRV$a   Anfangsmarke
 *      NTUDRV$m   die Eintraege
 *      NTUDRV$z   Endmarke
 *  und ntnewbus.c laeuft die Liste dazwischen ab.
 *
 *  Der Vorteil gegenueber einer handgepflegten Tabelle: der Treiber-
 *  quelltext bleibt unberuehrt, und ein neu hinzugenommener Treiber
 *  meldet sich von selbst an.  Das ist der ganze Sinn der Unterlage.
 *
 *  Beim Binden: /merge:NTUDRV=.rdata -- sonst kostet der Abschnitt eine
 *  eigene Seite.
 */
#ifndef _SYS_MODULE_H_
#define _SYS_MODULE_H_
#include <sys/bus.h>

#define DEVMETHOD(n, f)   { #n, (ntu_methodfn)(f) }
#define DEVMETHOD_END     { 0, 0 }

typedef struct ntu_drvreg {
    const char *name;
    const char *parent;
    driver_t   *driver;
} ntu_drvreg;

#pragma section("NTUDRV$a", read)
#pragma section("NTUDRV$m", read)
#pragma section("NTUDRV$z", read)

/*  __attribute__((used)) ist noetig, sonst wirft der Uebersetzer den
 *  scheinbar unbenutzten Zeiger weg und die Anmeldung verschwindet
 *  lautlos -- ein Treiber, der uebersetzt, bindet und nie gerufen wird.  */

#define DRIVER_MODULE(nm, busnm, dr, e, f)                                \
    static const ntu_drvreg __ntu_r_##nm##_##busnm = { #nm, #busnm, &dr };\
    __declspec(allocate("NTUDRV$m")) __attribute__((used))                \
    static const ntu_drvreg * const __ntu_p_##nm##_##busnm =              \
        &__ntu_r_##nm##_##busnm

#define DEFINE_CLASS_0(nm, dc, m, s) \
        driver_t dc = { #nm, (m), (s) }

#define MODULE_DEPEND(a,b,c,d,e)
#define MODULE_VERSION(a,b)
#endif
