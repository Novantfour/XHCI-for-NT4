/*
 * wolfSSL user_settings.h for Windows NT 4.0 SP6
 *
 * Target: Windows NT 4.0 SP6a, x86
 * Compiler: Microsoft Visual C++ 6.0 (MSVC 12.00) or NT4 DDK (C89)
 * Build: User-mode static library (wolfssl_nt4.lib)
 *
 * Provides: TLS 1.2 client and server capability on NT4
 *
 * Usage: Compile wolfSSL with:
 *   cl /DWOLFSSL_USER_SETTINGS /I. /Ipath\to\wolfssl ...
 *
 * (c) 2026 - NT4 Port
 */

#ifndef _NT4_USER_SETTINGS_H_
#define _NT4_USER_SETTINGS_H_

/* ===================================================================
 * 1. PLATFORM IDENTIFICATION
 * =================================================================== */

/* Force C89 mode - critical for MSVC 4.x/6.0 */
#define WOLF_C89

/* We are on Windows but NOT modern Windows */
/* _WIN32 should be defined by MSVC automatically */
#define USE_WINDOWS_API

/* NT4 does not have modern CryptoAPI, use wolfSSL's own RNG seeding */
#define WOLFSSL_NO_DEF_TICKET_ENC_CB

/* ===================================================================
 * 2. THREADING & SYNCHRONIZATION
 * =================================================================== */

/*
 * NT4 has threads (CreateThread, CRITICAL_SECTION) but wolfSSL's
 * Windows threading code uses some APIs not available on NT4.
 * Start single-threaded for safety; can be enabled later.
 */
#define SINGLE_THREADED

/* ===================================================================
 * 3. FILESYSTEM
 * =================================================================== */

/*
 * NT4 has a filesystem, so we can load certs/keys from files.
 * But we disable directory scanning (opendir/readdir not available).
 */
#define NO_WOLFSSL_DIR

/* ===================================================================
 * 4. NETWORK I/O
 * =================================================================== */

/*
 * NT4 SP6 has Winsock 2 (ws2_32.dll) with full socket support.
 * wolfSSL's default Windows I/O should work.
 */
#define USE_WOLFSSL_IO

/* No writev() on Windows */
#define NO_WRITEV

/* ===================================================================
 * 4b. NT4 API RESTRICTIONS
 * =================================================================== */

/*
 * NT4 does NOT have getaddrinfo() - it was added in Windows 2000.
 * wolfSSL falls back to gethostbyname() when this is not defined.
 * DO NOT define HAVE_GETADDRINFO!
 */
#ifdef HAVE_GETADDRINFO
    #undef HAVE_GETADDRINFO
#endif

/* NT4 does not have IPv6 */
/* #define WOLFSSL_IPV6 */

/* Prevent wolfSSL from using Windows certificate store APIs (Win2K+) */
/* #define WOLFSSL_SYS_CA_CERTS */

/* ===================================================================
 * 5. TLS PROTOCOL VERSIONS
 * =================================================================== */

/* Enable TLS 1.2 - the main goal of this port */
/* TLS 1.2 is enabled by default */

/* Disable TLS 1.3 - requires features beyond what NT4 can handle well */
#define NO_TLS13

/* Disable DTLS - not needed for basic TLS client/server */
/* #define NO_DTLS */

/* Allow older protocols for maximum compatibility */
/* SSLv3 disabled for security */
#define NO_OLD_TLS
/* But keep TLS 1.0 and 1.1 for servers that don't support 1.2 */
#undef NO_OLD_TLS
#define WOLFSSL_ALLOW_TLSV10

/* ===================================================================
 * 6. CIPHER SUITES
 * =================================================================== */

/* RSA - essential for TLS */
/* RSA is enabled by default */
#define WC_RSA_BLINDING
#define FP_MAX_BITS 4096

/* AES - primary symmetric cipher */
#define HAVE_AESGCM
#define HAVE_AES_CBC

/* SHA-256/384/512 - needed for TLS 1.2 */
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512

/* ECC - modern key exchange */
#define HAVE_ECC
#define ECC_SHAMIR
#define ECC_TIMING_RESISTANT
#define HAVE_SUPPORTED_CURVES
#define HAVE_TLS_EXTENSIONS

/* Diffie-Hellman */
/* DH is enabled by default */

/* ChaCha20-Poly1305 - good alternative cipher suite */
#define HAVE_CHACHA
#define HAVE_POLY1305

/* HKDF for key derivation */
#define HAVE_HKDF

/* Extended Master Secret - TLS security improvement */
#define HAVE_EXTENDED_MASTER

/* SNI extension - needed for modern HTTPS */
#define HAVE_SNI

/* Session resumption */
/* Session cache is enabled by default */

/* ===================================================================
 * 7. DISABLED FEATURES (not needed / not available on NT4)
 * =================================================================== */

/* No PSK (Pre-Shared Key) */
#define NO_PSK

/* No RC4 (insecure) */
#define NO_RC4

/* No MD4 (insecure, unused) */
#define NO_MD4

/* No DSA */
#define NO_DSA

/* No DES3 (insecure) */
/* #define NO_DES3 */

/* No PKCS#7 */
/* #define NO_PKCS7 */

/* No OCSP (requires network callbacks NT4 may struggle with) */
/* #define NO_OCSP */

/* No CRL initially */
/* #define NO_CRL */

/* ===================================================================
 * 8. MATH LIBRARY
 * =================================================================== */

/*
 * Use SP Math (Single Precision) - wolfSSL's own math library.
 * This avoids external dependencies and is constant-time.
 * WOLF_C89 + SP Math works well together.
 */
#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_SP_SMALL
#define SP_INT_BITS 4096

/* No dynamic stack allocation (alloca) - C89 safety */
#define WOLFSSL_SP_NO_DYN_STACK

/* No inline assembly for old MSVC */
#define WOLFSSL_NO_ASM
#define NO_INLINE

/* ===================================================================
 * 9. MEMORY
 * =================================================================== */

/* Use small stack to avoid stack overflows on NT4 */
#define WOLFSSL_SMALL_STACK

/* Standard malloc/free from MSVCRT */
/* No custom allocator needed */

/* ===================================================================
 * 10. RANDOM NUMBER GENERATION
 * =================================================================== */

/*
 * NT4 does not have CryptGenRandom in the same way as modern Windows.
 * However, NT4 SP6 does have the Microsoft Base Cryptographic Provider
 * (advapi32.dll: CryptAcquireContext, CryptGenRandom).
 * wolfSSL's USE_WINDOWS_API path should handle this via wc_port.c.
 *
 * If CryptGenRandom is not available, define CUSTOM_RAND_GENERATE_BLOCK
 * and implement your own.
 */

/* Hash-based DRBG on top of OS entropy */
#define HAVE_HASHDRBG

/* ===================================================================
 * 11. CERTIFICATE HANDLING
 * =================================================================== */

/* Enable certificate generation/verification */
#define WOLFSSL_CERT_GEN
#define WOLFSSL_KEY_GEN
#define WOLFSSL_CERT_EXT

/* Allow PEM format */
/* PEM is enabled by default */

/* Allow DER format */
/* DER is enabled by default */

/* ===================================================================
 * 12. C89 COMPATIBILITY WORKAROUNDS
 * =================================================================== */

/* Suppress C99 features that MSVC 6.0 doesn't have */
#define WOLF_NO_VARIADIC_MACROS

/* No snprintf in old MSVC - use _snprintf */
#if defined(_MSC_VER) && (_MSC_VER < 1400)
    #define XSNPRINTF _snprintf
#endif

/* No vsnprintf in old MSVC - use _vsnprintf */
#if defined(_MSC_VER) && (_MSC_VER < 1400)
    #define XVSNPRINTF _vsnprintf
#endif

/* Disable features that require C99 */
#define NO_MULTIBYTE_PRINT

/* ===================================================================
 * 13. DEBUG (optional, disable for production)
 * =================================================================== */

/* Uncomment for debug output */
/* #define DEBUG_WOLFSSL */

/* Uncomment for verbose error strings */
#define WOLFSSL_ERROR_CODE_OPENSSL
/* #define OPENSSL_EXTRA */

/* ===================================================================
 * 14. NT4-SPECIFIC COMPAT LAYER
 * =================================================================== */

/*
 * If building with the NT4 compatibility shim (nt4_compat.h),
 * this ensures the right APIs are used.
 */
#define WOLFSSL_NT4

/*
 * NT4 Winsock2 headers are missing some types that wolfSSL needs.
 * We define them here so they exist before wolfSSL headers parse.
 */
#ifdef WOLFSSL_NT4

    /*
     * struct sockaddr_storage - missing from NT4 winsock2.h
     * Introduced in Windows 2000 / RFC 2553.
     * wolfSSL uses this as SOCKADDR_S in wolfio.h.
     * Only define macros for real NT4 SDK. MinGW has its own.
     */
    #if defined(_MSC_VER) && (_MSC_VER < 1400)
    #ifndef _SS_MAXSIZE
    #define _SS_MAXSIZE    128
    #define _SS_ALIGNSIZE  (sizeof(__int64))
    #define _SS_PAD1SIZE   (_SS_ALIGNSIZE - sizeof(short))
    #define _SS_PAD2SIZE   (_SS_MAXSIZE - (sizeof(short) + \
                            _SS_PAD1SIZE + _SS_ALIGNSIZE))
    #endif
    #endif /* _MSC_VER < 1400 */

    /*
     * socklen_t - not in NT4 winsock2.h (Winsock uses int)
     * MinGW already defines this.
     */
    #if defined(_MSC_VER) && !defined(_SOCKLEN_T_DEFINED)
    #define _SOCKLEN_T_DEFINED
    typedef int socklen_t;
    #endif

#endif /* WOLFSSL_NT4 */

#endif /* _NT4_USER_SETTINGS_H_ */
