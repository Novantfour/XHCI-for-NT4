# pad.sh "A 1" "LX 20000" ...  -- Zeilen an den Emulator
python3 - "$@" <<'P'
import socket,sys
s=socket.socket(socket.AF_UNIX); s.connect(__import__('os').environ.get('PADCTL','/tmp/pad.ctl')); f=s.makefile('rw')
for z in sys.argv[1:]:
    f.write(z+'\n'); f.flush(); print(z,'->',f.readline().strip())
P
