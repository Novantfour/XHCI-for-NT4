/*  ctype.h -- Rumpf.  ntdef.h zieht es, benutzt es aber nicht; die
 *  Zeichenpruefungen des Treibers stehen in sys/ctype.h der Unterlage.  */
#ifndef _INC_CTYPE
#define _INC_CTYPE

/*  MSVC definiert wchar_t hier, nicht in stddef.h -- ntdef.h verlaesst
 *  sich darauf.  In C ist es unsigned short.  */
#ifndef _WCHAR_T_DEFINED
#define _WCHAR_T_DEFINED
typedef unsigned short wchar_t;
#endif
int __cdecl isdigit(int);
int __cdecl isspace(int);
int __cdecl isalpha(int);
int __cdecl isupper(int);
int __cdecl islower(int);
int __cdecl toupper(int);
int __cdecl tolower(int);
#endif
