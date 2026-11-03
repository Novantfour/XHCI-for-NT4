/*  excpt.h -- Rumpf.
 *
 *  Gehoert zum Win32-SDK, nicht zum DDK; auf dieser Maschine liegt es
 *  nicht.  Der Treiber benutzt keine strukturierte Ausnahmebehandlung
 *  -- try/except gibt es im ganzen Baum nicht -- also reicht, was
 *  ntddk.h zum Uebersetzen braucht.
 *
 *  Sollte spaeter doch jemand __try schreiben: dann diese Datei durch
 *  das echte excpt.h ersetzen, nicht erweitern.
 */
#ifndef _INC_EXCPT
#define _INC_EXCPT
typedef enum _EXCEPTION_DISPOSITION {
    ExceptionContinueExecution,
    ExceptionContinueSearch,
    ExceptionNestedException,
    ExceptionCollidedUnwind
} EXCEPTION_DISPOSITION;
#define GetExceptionCode        _exception_code
#define GetExceptionInformation ((struct _EXCEPTION_POINTERS *)_exception_info())
#define AbnormalTermination     _abnormal_termination
unsigned long __cdecl _exception_code(void);
void *        __cdecl _exception_info(void);
int           __cdecl _abnormal_termination(void);
#define EXCEPTION_EXECUTE_HANDLER     1
#define EXCEPTION_CONTINUE_SEARCH     0
#define EXCEPTION_CONTINUE_EXECUTION -1
#endif
