/* ausschalten.exe -- NT 4.0 sauber herunterfahren, OHNE Neustart (Pruefstand). */
#include <windows.h>
#include "ntbasis.h"
int main(void){
    HANDLE t; TOKEN_PRIVILEGES p;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY, &t) &&
        LookupPrivilegeValueA(NULL, "SeShutdownPrivilege", &p.Privileges[0].Luid)) {
        p.PrivilegeCount = 1; p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(t, FALSE, &p, 0, NULL, NULL);
    }
    if (!InitiateSystemShutdownA(NULL, NULL, 0, TRUE, FALSE)) { printf("Fehler %lu\n", GetLastError()); return 1; }
    printf("faehrt herunter\n"); return 0;
}
