#!/usr/bin/env python3
# autologon.py -- AutoAdminLogon im laufenden Abbild wieder auf 1 (NT setzt
# es bei leerem Kennwort nach dem ersten Gebrauch zurueck).  Nur bei
# angehaltener VM benutzen.
import hivex, subprocess
subprocess.run('dd if=nt4_smal.img of=part.ntfs bs=512 skip=63 count=1048257 status=none', shell=True, check=True)
subprocess.run('ntfscat part.ntfs /WINNT/system32/config/software > /tmp/sw', shell=True, check=True)
h=hivex.Hivex('/tmp/sw', write=True); n=h.root()
for t in 'Microsoft\\Windows NT\\CurrentVersion\\Winlogon'.split('\\'): n=h.node_get_child(n,t)
alt=[{'key':h.value_key(v),'t':h.value_type(v)[0],'value':h.value_value(v)[1]} for v in h.node_values(n) if h.value_key(v)!='AutoAdminLogon']
alt.append({'key':'AutoAdminLogon','t':1,'value':'1\0'.encode('utf-16-le')})
h.node_set_values(n, alt); h.commit(None)
subprocess.run(['ntfscp','-f','part.ntfs','/tmp/sw','/WINNT/system32/config/software'], check=True)
subprocess.run('dd if=part.ntfs of=nt4_smal.img bs=512 seek=63 conv=notrunc status=none', shell=True, check=True)
print('AutoAdminLogon = 1')
