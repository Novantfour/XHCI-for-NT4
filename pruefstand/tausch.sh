#!/bin/sh
# tausch.sh <treiber.sys>: frisches Abbild, nur usbnt.sys ersetzt (keine Hive-Aenderung)
cd /home/claude/vm
cp nt4_orig.img nt4_smal.img
dd if=nt4_smal.img of=part.ntfs bs=512 skip=63 count=1048257 status=none
ntfscp -f part.ntfs "$1" /WINNT/system32/drivers/usbnt.sys
dd if=part.ntfs of=nt4_smal.img bs=512 seek=63 conv=notrunc status=none
