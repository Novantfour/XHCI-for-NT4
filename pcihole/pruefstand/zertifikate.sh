#!/bin/sh
# Test-CA und Serverzertifikate: gut (localhost), falsch (anderer Name),
# alt (abgelaufen), selbst (nicht von der CA).  CACERT.PEM = Mozilla + Test-CA.
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 3650 -subj "/CN=Pruefstand Test-CA"
mk() { openssl req -newkey rsa:2048 -nodes -keyout $1.key -out $1.csr -subj "/CN=$2"
       printf "subjectAltName=DNS:$2\nbasicConstraints=CA:FALSE\n" > $1.ext
       openssl x509 -req -in $1.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out $1.pem $3 -extfile $1.ext; }
mk gut localhost "-days 365"; mk falsch falsch.example "-days 365"; mk alt localhost "-days -1"
openssl req -x509 -newkey rsa:2048 -nodes -keyout selbst.key -out selbst.pem -days 365 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost"
cat ../CACERT.PEM ca.pem > CACERT.PEM
# Dann: python3 tlsserver.py &  und unter Wine:
#   wine PCIHOLE.EXE https://localhost:8443/pci.ids z.ids   (gut, 8444 falsch, 8445 alt, 8446 selbst)
