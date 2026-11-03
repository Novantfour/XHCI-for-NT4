/*  string.h -- Rumpf.  Die Umsetzungen liegen in NTOSKRNL und sind in
 *  sys/libkern.h der Unterlage schon erklaert; hier steht nur, was
 *  ntddk.h selbst braucht.  */
#ifndef _INC_STRING
#define _INC_STRING
void * __cdecl memcpy(void *, const void *, unsigned int);
void * __cdecl memmove(void *, const void *, unsigned int);
void * __cdecl memset(void *, int, unsigned int);
int    __cdecl memcmp(const void *, const void *, unsigned int);
unsigned int __cdecl strlen(const char *);
char * __cdecl strcpy(char *, const char *);
int    __cdecl strcmp(const char *, const char *);
#endif
