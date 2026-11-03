#!/bin/sh
# stop.sh -- VM ueber die PID-Datei beenden.  NICHT pkill -f: das trifft
# jede Shell, deren Befehlszeile den Programmnamen enthaelt.
cd /home/claude/vm
[ -f qemu.pid ] && kill $(cat qemu.pid) 2>/dev/null
sleep 1
rm -f qemu.pid
