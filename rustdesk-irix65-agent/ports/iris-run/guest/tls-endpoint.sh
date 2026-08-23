#!/bin/sh
# Runs on the HOST, not the guest.
#
# Stands up an HTTPS endpoint with a certificate we control, so the IRIX build's
# TLS path can be exercised end to end. That path is the whole of --api-server:
# the console heartbeat is an HTTPS POST, and nothing had ever put a real
# handshake through mbedTLS on this target -- its own selftest passing and a
# plain TCP GET working are neither of them the same claim.
#
# The self-signed cert with an IP SAN is deliberately the same shape as a
# self-hosted console behind a private CA, which is the case --ca-bundle exists
# for.
set -e
here=$(cd "$(dirname "$0")" && pwd)
d="${TLSDIR:-/tmp/rd-tls-endpoint}"
mkdir -p "$d"; cd "$d"
if [ ! -f cert.pem ]; then
    openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 30 \
        -nodes -subj "/CN=192.168.0.1" -addext "subjectAltName=IP:192.168.0.1" 2>/dev/null
fi
# The guest fetches this next to the binaries, as /tmp/testca.pem.
cp cert.pem "$here"/../../rust/agent-portable/target/mips-sgi-irix6.5/release/testca.pem
cat > server.py <<'PY'
import http.server, ssl, json
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        print("POST %s %d bytes: %s" % (self.path, n, self.rfile.read(n)[:120]), flush=True)
        out = json.dumps({"modified_at": 0}).encode()
        self.send_response(200); self.send_header("Content-Type","application/json")
        self.send_header("Content-Length", str(len(out))); self.end_headers(); self.wfile.write(out)
    def do_GET(self):
        out = b'{"ok":true}'
        self.send_response(200); self.send_header("Content-Type","application/json")
        self.send_header("Content-Length", str(len(out))); self.end_headers(); self.wfile.write(out)
    def log_message(self, *a): pass
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain("cert.pem","key.pem")
srv = http.server.HTTPServer(("0.0.0.0", 8443), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print("https on :8443", flush=True); srv.serve_forever()
PY
echo "serving https://192.168.0.1:8443 from $d"
exec python3 server.py
