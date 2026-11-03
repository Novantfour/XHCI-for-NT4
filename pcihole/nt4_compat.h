/*
 * nt4_compat.h - Windows NT 4.0 SP6 Compatibility Layer for wolfSSL
 *
 * Provides missing C runtime functions and Win32 API wrappers
 * that wolfSSL expects but NT4/MSVC 6.0 doesn't have.
 *
 * Include this AFTER wolfSSL headers or in your application code.
 */

#ifndef _NT4_COMPAT_H_
#define _NT4_COMPAT_H_

#ifdef WOLFSSL_NT4

#include <windows.h>
#include <wincrypt.h>  /* CryptAcquireContext, CryptGenRandom */
#include <winsock2.h>  /* Must come after windows.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * 0. MISSING WINSOCK2 TYPES (not in NT4 SP6 ws2_32.dll)
 * =================================================================== */

/*
 * struct sockaddr_storage was introduced in Windows 2000 / RFC 2553.
 * NT4's winsock2.h does not define it. wolfSSL's wolfio.h uses it
 * as SOCKADDR_S for generic address storage.
 *
 * Only define for real NT4 SDK (MSVC < VS2005). MinGW has its own.
 */
#if defined(_MSC_VER) && (_MSC_VER < 1400)
#ifndef _SS_MAXSIZE
#define _SS_MAXSIZE    128
#define _SS_ALIGNSIZE  (sizeof(__int64))
#define _SS_PAD1SIZE   (_SS_ALIGNSIZE - sizeof(short))
#define _SS_PAD2SIZE   (_SS_MAXSIZE - (sizeof(short) + _SS_PAD1SIZE + _SS_ALIGNSIZE))

struct sockaddr_storage {
    short ss_family;
    char __ss_pad1[_SS_PAD1SIZE];
    __int64 __ss_align;
    char __ss_pad2[_SS_PAD2SIZE];
};
#endif /* _SS_MAXSIZE */
#endif /* _MSC_VER < 1400 */

/*
 * socklen_t is not defined in NT4 winsock2.h
 * Winsock uses int for address length parameters.
 * MinGW already defines this.
 */
#if defined(_MSC_VER) && !defined(_SOCKLEN_T_DEFINED)
#define _SOCKLEN_T_DEFINED
typedef int socklen_t;
#endif

/* ===================================================================
 * 1. MISSING STRING FUNCTIONS
 * =================================================================== */

/*
 * strncasecmp / _strnicmp
 * MSVC 6.0 has _strnicmp, but some wolfSSL code paths
 * may reference strncasecmp (POSIX name).
 */
#ifndef strncasecmp
    #define strncasecmp _strnicmp
#endif

#ifndef strcasecmp
    #define strcasecmp _stricmp
#endif

/* ===================================================================
 * 2. GETENV WRAPPER
 * =================================================================== */

/*
 * NT4 has getenv() in MSVCRT. No special handling needed.
 */

/* ===================================================================
 * 3. TIME FUNCTIONS
 * =================================================================== */

/*
 * NT4 MSVCRT has time(), gmtime(), mktime().
 * wolfSSL needs these for certificate validation.
 *
 * If gmtime_r is needed, we provide a simple wrapper.
 * Note: NOT thread-safe on NT4, but we're SINGLE_THREADED.
 */
#ifndef HAVE_GMTIME_R
static struct tm* nt4_gmtime_r(const time_t *timer, struct tm *result)
{
    struct tm *gmt;
    if (timer == NULL || result == NULL) {
        return NULL;
    }
    gmt = gmtime(timer);
    if (gmt == NULL) {
        return NULL;
    }
    memcpy(result, gmt, sizeof(struct tm));
    return result;
}
#define gmtime_r nt4_gmtime_r
#define HAVE_GMTIME_R
#endif

/* ===================================================================
 * 4. ENTROPY / RANDOM NUMBER GENERATION
 * =================================================================== */

/*
 * NT4 SP6 with High Encryption Pack has CryptGenRandom in advapi32.dll.
 * This is used by wolfSSL's wc_port.c via USE_WINDOWS_API.
 *
 * If for some reason CryptGenRandom fails or is not available,
 * provide a fallback using system state. THIS IS LESS SECURE
 * but better than nothing for testing.
 */

/*
 * NT4 CryptGenRandom usage:
 *
 * HCRYPTPROV hProv;
 * CryptAcquireContext(&hProv, NULL, NULL,
 *                     PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
 * CryptGenRandom(hProv, len, buf);
 * CryptReleaseContext(hProv, 0);
 *
 * wolfSSL's wc_port.c already does this when USE_WINDOWS_API is defined.
 * We link against advapi32.lib to get these functions.
 */

/* ===================================================================
 * 5. SOCKET COMPATIBILITY
 * =================================================================== */

/*
 * NT4 SP6 has Winsock 2 (ws2_32.dll).
 * wolfSSL's wolfio.c handles Windows sockets via USE_WINDOWS_API.
 *
 * Make sure to call WSAStartup() before using wolfSSL!
 *
 * Example:
 *   WSADATA wsaData;
 *   WSAStartup(MAKEWORD(2,0), &wsaData);
 *   wolfSSL_Init();
 *   ... use wolfSSL ...
 *   wolfSSL_Cleanup();
 *   WSACleanup();
 */

/* ===================================================================
 * 6. INTERLOCKED FUNCTIONS
 * =================================================================== */

/*
 * NT4 has InterlockedIncrement/InterlockedDecrement in kernel32.dll.
 * These work fine. No shim needed.
 */

/* ===================================================================
 * 7. MISSING MSVC 6.0 INTRINSICS / BUILTINS
 * =================================================================== */

/*
 * MSVC 6.0 does not have:
 * - __debugbreak() -> use DebugBreak() from kernel32
 * - _BitScanForward/Reverse -> implement manually
 * - __int64 is supported but some C99 macros are not
 */

/* Provide 64-bit printf format if needed */
#if defined(_MSC_VER) && (_MSC_VER < 1400)
    #ifndef PRId64
        #define PRId64 "I64d"
    #endif
    #ifndef PRIu64
        #define PRIu64 "I64u"
    #endif
    #ifndef PRIx64
        #define PRIx64 "I64x"
    #endif
#endif

/* ===================================================================
 * 8. ERRNO HANDLING
 * =================================================================== */

/*
 * NT4 MSVCRT has errno. wolfSSL uses errno for some error paths.
 * No special handling needed.
 */

/* ===================================================================
 * 9. LARGE FILE SUPPORT
 * =================================================================== */

/*
 * NT4 supports large files via SetFilePointer with LARGE_INTEGER.
 * wolfSSL doesn't typically need this for TLS operations.
 * No special handling needed.
 */

/* ===================================================================
 * 10. INITIALIZATION HELPER
 * =================================================================== */

/*
 * Call this before wolfSSL_Init() to set up NT4-specific state.
 * Returns 0 on success, non-zero on failure.
 */
static int nt4_wolfssl_pre_init(void)
{
    WSADATA wsaData;
    int ret;

    /* Initialize Winsock 2 */
    ret = WSAStartup(MAKEWORD(2, 0), &wsaData);
    if (ret != 0) {
        return ret;
    }

    return 0;
}

/*
 * Call this after wolfSSL_Cleanup() to tear down NT4-specific state.
 */
static void nt4_wolfssl_post_cleanup(void)
{
    WSACleanup();
}

#ifdef __cplusplus
}
#endif

#endif /* WOLFSSL_NT4 */
#endif /* _NT4_COMPAT_H_ */
