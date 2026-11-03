import http.server, socketserver, ssl, sys, threading
class H(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.0'
    def log_message(self,*a): pass
    def antwort(self,code,kopf=None,rumpf=b''):
        self.send_response(code)
        for k,v in (kopf or {}).items(): self.send_header(k,v)
        self.send_header('Content-Length',str(len(rumpf))); self.end_headers(); self.wfile.write(rumpf)
    def do_GET(self):
        p=self.path
        if p=='/pci.ids': self.antwort(200,{},open('pci.ids','rb').read())
        elif p=='/umleitung': self.antwort(302,{'Location':'/pci.ids'})
        elif p=='/aufhttp': self.antwort(301,{'Location':'http://localhost:8765/pci.ids'})
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
def starte(port,zert):
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(zert+'.pem',zert+'.key')
    s=socketserver.ThreadingTCPServer(('127.0.0.1',port),H); s.socket=ctx.wrap_socket(s.socket,server_side=True)
    threading.Thread(target=s.serve_forever,daemon=True).start()
for port,z in ((8443,'gut'),(8444,'falsch'),(8445,'alt'),(8446,'selbst')): starte(port,z)
threading.Event().wait()
