#ifndef _SYS_LIBKERN_H_
#define _SYS_LIBKERN_H_
#include <sys/types.h>
/*  Alle echte Exporte von NTOSKRNL -- gegen tools/ntkrnlmp.def und
 *  gegen die NTOSKRNL.LIB des DDK geprueft, nicht aus dem Gedaechtnis.
 *
 *  __cdecl muss dranstehen.  Gebaut wird mit /Gz, also stdcall als
 *  Vorgabe -- ohne die Angabe verlangt der Binder __vsnprintf@16, und
 *  die Bibliothek hat _vsnprintf.  Der Fehler faellt erst beim Binden
 *  auf, und die Meldung nennt nur den verzierten Namen.  */
#ifndef __cdecl
#ifndef _MSC_VER
#define __cdecl
#endif
#endif
__cdecl void  *memset (void *, int, size_t);
__cdecl void  *memcpy (void *, const void *, size_t);
__cdecl void  *memmove(void *, const void *, size_t);
__cdecl int    memcmp (const void *, const void *, size_t);
__cdecl size_t strlen (const char *);
__cdecl int    strcmp (const char *, const char *);
__cdecl int    strncmp(const char *, const char *, size_t);
__cdecl int    sprintf(char *, const char *, ...);
__cdecl int    _snprintf(char *, size_t, const char *, ...);
__cdecl int    _vsnprintf(char *, size_t, const char *, void *);
#define bzero(p,n)   memset((p), 0, (n))
#define bcopy(s,d,n) memmove((d), (s), (n))
size_t strlcpy(char *, const char *, size_t);
#endif
