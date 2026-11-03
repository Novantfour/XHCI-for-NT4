# aus.sh -- Gast ueber den Agenten sauber ausschalten (ohne Neustart), dann QEMU beenden
export NTAGENT_SOCK=/tmp/com2.sock
timeout 30 python3 ntagent_v3_1/ag3.py 'C:\TEMP\ausschalten.exe' >/dev/null 2>&1
for i in $(seq 1 20); do sleep 3; python3 mon.py screendump /tmp/z.ppm >/dev/null 2>&1
  python3 -c "
from PIL import Image;im=Image.open('/tmp/z.ppm');import sys
sys.exit(0 if len(set(im.crop((250,150,550,300)).getdata()))<40 else 1)" 2>/dev/null && break; done
sleep 3; sh halt.sh
