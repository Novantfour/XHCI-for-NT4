#!/usr/bin/env python3
"""Bettet IPHLPAPI.DLL und IPCHECK.EXE als Bytefelder in payload.h ein."""
import sys

def emit(f, name, path):
    d = open(path, 'rb').read()
    f.write("static const BYTE %s[] = {\n" % name)
    for i in range(0, len(d), 16):
        f.write("    " + ",".join("0x%02x" % b for b in d[i:i+16]) + ",\n")
    f.write("};\n")
    f.write("static const DWORD %sLen = %d;\n\n" % (name, len(d)))
    return len(d)

out = sys.argv[1]
with open(out, 'w') as f:
    f.write("/* Erzeugt von tools/mkpayload.py -- nicht von Hand aendern. */\n")
    f.write("#ifndef _DRVPAYLOAD_H_\n#define _DRVPAYLOAD_H_\n\n")
    a = emit(f, "g_Drv", sys.argv[2])
    f.write("#endif\n")
print("drvpayload.h: Treiber %d Bytes" % a)
