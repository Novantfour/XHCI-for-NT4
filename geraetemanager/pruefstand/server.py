import http.server, socketserver
class H(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.0'
    def log_message(self,*a): pass
    def antwort(self,code,kopf=None,rumpf=b''):
        self.send_response(code)
        for k,v in (kopf or {}).items(): self.send_header(k,v)
        self.send_header('Content-Length',str(len(rumpf))); self.end_headers(); self.wfile.write(rumpf)
    def do_GET(self):
        p=self.path
        if p=='/pci.ids': self.antwort(200,{'Content-Type':'text/plain'},open('pci.ids','rb').read())
        elif p=='/relativ': self.antwort(302,{'Location':'/pci.ids'})
        elif p=='/absolut': self.antwort(301,{'Location':'http://127.0.0.1:8765/pci.ids'})
        elif p=='/https': self.antwort(301,{'Location':'https://pci-ids.ucw.cz/v2.2/pci.ids'})
        elif p=='/klein': self.antwort(200,{},b'# nur ein Kommentar\n8086  Intel Corporation\n')
        elif p=='/schleife': self.antwort(302,{'Location':'/schleife'})
        elif p=='/stuecke':
            d=open('pci.ids','rb').read(); self.send_response(200)
            self.send_header('Transfer-Encoding','chunked'); self.end_headers()
            i=0; g=[1,7,4096,65535,300,1]
            while i<len(d):
                n=g[i%len(g)] if i<200000 else 50000
                t=d[i:i+n]; self.wfile.write(b'%x;erweiterung=1\r\n'%len(t)+t+b'\r\n'); i+=n
            self.wfile.write(b'0\r\n\r\n')
        elif p=='/stuecke_kaputt':
            self.send_response(200); self.send_header('Transfer-Encoding','chunked'); self.end_headers()
            self.wfile.write(b'ffff\r\nzu kurz')
        else: self.antwort(404,{},b'nicht da')
socketserver.TCPServer.allow_reuse_address=True
socketserver.TCPServer(('127.0.0.1',8765),H).serve_forever()
