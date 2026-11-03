#!/bin/sh
#  PCIHOLE.EXE bauen: HTTPS-Abruf fuer den Geraetemanager, wolfSSL 5.9.1.
#  wolfssl_nt4.a und user_settings.h stammen aus wolfssl_nt4_build.zip;
#  die Kopfdateien aus wolfSSL v5.9.1-stable (WOLFSSL=...).
#  Achtung: die Bibliothek importiert strcpy_s/strtok_s, die msvcrt.dll von
#  NT 4.0 nicht hat -- pcihole.c liefert eigene ueber die Importzeiger.
set -e
cd "$(dirname "$0")"
WOLFSSL=${WOLFSSL:-/tmp/wolfssl-src}
[ -f $WOLFSSL/wolfssl/ssl.h ] || { mkdir -p $WOLFSSL; curl -sSfL https://codeload.github.com/wolfSSL/wolfssl/tar.gz/refs/tags/v5.9.1-stable | tar xz -C $WOLFSSL --strip-components=1; }
i686-w64-mingw32-windres -c 1252 pcihole.rc -O coff -o pcihole_res.o
i686-w64-mingw32-gcc -Os -Wall -Wno-unused-function -fno-asynchronous-unwind-tables -DWOLFSSL_USER_SETTINGS -I. -I$WOLFSSL -o PCIHOLE.EXE pcihole.c pcihole_res.o wolfssl_nt4.a \
  -lws2_32 -ladvapi32 -Wl,--major-subsystem-version=4 -Wl,--minor-subsystem-version=0 -Wl,--major-os-version=4 -Wl,--minor-os-version=0
#  .eh_frame (70 KB aus wolfssl_nt4.a) wird in reinem C nie benutzt; nach
#  strip hiesse der Abschnitt "/4" -- so etwas erzeugt kein Microsoft-Linker,
#  Virenscanner werten es als Auffaelligkeit.  Also vor strip entfernen.
i686-w64-mingw32-objcopy --remove-section=.eh_frame PCIHOLE.EXE
i686-w64-mingw32-strip PCIHOLE.EXE
#  Zertifikatsliste: Mozilla, ueber das certifi-Projekt
[ -f CACERT.PEM ] || curl -sSfL -o CACERT.PEM https://raw.githubusercontent.com/certifi/python-certifi/master/certifi/cacert.pem
ls -la PCIHOLE.EXE CACERT.PEM
