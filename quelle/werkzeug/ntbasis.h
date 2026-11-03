/*  werkzeug/ntbasis.h -- was ntbasis.c anbietet.  Ersetzt stdio.h,
 *  stdlib.h und string.h; siehe den Kopf von ntbasis.c fuer den Grund.  */
#ifndef _NTBASIS_H_
#define _NTBASIS_H_
int   printf(const char *, ...);
int   sprintf(char *, const char *, const char *);
void *memset(void *, int, unsigned int);
void *memcpy(void *, const void *, unsigned int);
unsigned int strlen(const char *);
int   strcmp(const char *, const char *);
char *strcpy(char *, const char *);
char *strrchr(const char *, int);
char *strchr(const char *, int);
int   strncmp(const char *, const char *, unsigned int);
int   atoi(const char *);
#endif
