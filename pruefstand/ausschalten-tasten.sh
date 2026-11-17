#!/bin/sh
# Anmelden (leeres Kennwort), Meldungen wegklicken, Start > Beenden > Herunterfahren
cd /home/claude/vm
k(){ python3 mon.py "sendkey $1" >/dev/null; timeout ${2:-2} tail -f /dev/null; }
k ctrl-alt-delete 4; k ret 15; k ret; k esc; k ret; k esc; k ret
k ctrl-esc; k up 1; k ret 3; k up 1; k up 1; k ret 60
