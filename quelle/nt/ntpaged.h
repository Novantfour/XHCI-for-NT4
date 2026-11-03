/*
 *  nt/ntpaged.h -- Auslagerbarer Code auf NT 4.0.
 *
 *  Hinweis von Ray Hinchliffe (SIV): Code, der garantiert nur auf
 *  PASSIVE_LEVEL laeuft, gehoert in einen auslagerbaren Abschnitt PAGE,
 *  und jede solche Routine traegt PAGED_CODE() als erste Anweisung.  Das
 *  gibt den Speicher frei, solange die Routine nicht gebraucht wird; der
 *  residente Teil (ISR, DPC, alles ab DISPATCH_LEVEL) bleibt im RAM.
 *
 *  Umsetzung hier:
 *    NTU_PAGED(fn)   vor der Definition  -> legt fn in den Abschnitt PAGE
 *    NTU_PAGED_CODE()  erste Anweisung   -> Debug: prueft, dass IRQL
 *                                           hoechstens APC_LEVEL ist
 *
 *  clang-cl kennt #pragma alloc_text; lld-link legt den Abschnitt PAGE
 *  an.  Das Bauskript merged PAGE NICHT mehr in .text, sondern laesst ihn
 *  als eigenen, auslagerbaren Abschnitt stehen.
 *
 *  WICHTIG: Nur auf Routinen anwenden, die NIE im DPC-/ISR-Pfad laufen.
 *  Beim FreeBSD-Stapel ist das die Minderheit (Einrichten, Entladen,
 *  Registry); die IRP-Dispatch-Pfade koennen ein Interrupt-Transfer im
 *  DPC abschliessen und bleiben deshalb resident.
 */

#ifndef _NT_NTPAGED_H_
#define _NT_NTPAGED_H_

#include <ntddk.h>

/*  Abschnittszuordnung.  __declspec(code_seg) waere der moderne Weg,
 *  aber clang-cl fuer NT 4.0 nimmt zuverlaessig #pragma alloc_text.
 *  Das Makro wird VOR der Routine benutzt und braucht ihren Namen.  */
#define NTU_PAGED(fn)   __pragma(alloc_text(PAGE, fn))

/*  Erste Anweisung einer auslagerbaren Routine.  Frei: leer.  Debug
 *  (NTU_DBG gesetzt): bricht ab, wenn die Routine ueber APC_LEVEL laeuft
 *  -- dann waere sie faelschlich ausgelagert.  */
#if defined(NTU_DBG)
#define NTU_PAGED_CODE()                                                   \
    do {                                                                   \
        if (KeGetCurrentIrql() > APC_LEVEL) {                              \
            ntu_printf("PAGED: Routine auf IRQL %u gerufen!\n",            \
                       KeGetCurrentIrql());                                \
        }                                                                  \
    } while (0)
#else
#define NTU_PAGED_CODE()   ((void)0)
#endif

#endif
