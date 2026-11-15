/*
 *  nt/ntfreiekoepfe.h -- Ausgleich fuer den Bau mit den freien DDK-Koepfen
 *  von mingw-w64 statt des NT-4.0-DDK (Schalter NTU_FREIE_KOEPFE).
 *
 *  Nur Stellen, an denen die freien Koepfe anders sind als das DDK; der
 *  erzeugte Code muss derselbe bleiben.  Was NT 4.0 nicht exportiert,
 *  faellt spaeter beim Binden gegen die echten Exportlisten auf.
 */
#ifndef _NT_FREIEKOEPFE_H_
#define _NT_FREIEKOEPFE_H_

/*  mingw-w64: Makro ohne Klammern um das Argument -> &x->State.  */
#undef  KeInitializeCallbackRecord
#define KeInitializeCallbackRecord(r)  ((r)->State = BufferEmpty)

#endif
