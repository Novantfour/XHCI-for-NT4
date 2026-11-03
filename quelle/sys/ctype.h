#ifndef _SYS_CTYPE_H_
#define _SYS_CTYPE_H_
#define isdigit(c) ((c) >= '0' && (c) <= '9')
#define isspace(c) ((c) == ' ' || ((c) >= 9 && (c) <= 13))
#define isalpha(c) ((((c)|0x20) >= 'a') && (((c)|0x20) <= 'z'))
#define toupper(c) (((c) >= 'a' && (c) <= 'z') ? ((c) - 32) : (c))
#define tolower(c) (((c) >= 'A' && (c) <= 'Z') ? ((c) + 32) : (c))
#endif
