#!/bin/sh
# bild.sh <name>  -- Bildschirm der VM als PNG
cd /home/claude/vm
python3 mon.py screendump /tmp/sd.ppm > /dev/null
sleep 1
python3 -c "
from PIL import Image
Image.open('/tmp/sd.ppm').save('/home/claude/vm/$1.png')
"
echo /home/claude/vm/$1.png
