#!/usr/bin/env python3
# T08 connection-time network policy driver (runs inside the QEMU guest).
#
# The host has real root, so it controls /etc/hosts (forcing the system
# resolver to hand the provider private/public addresses) and owns loopback
# addresses that the provider's public-address policy sees. Two public
# listeners on 1.1.1.1 (HTTPS for the fetch origin, HTTP for a media source)
# and two private listeners on 10.99.0.1 (443/8000) that record any inbound
# connection. Drives the real network_media_provider (no TESTING short-circuits,
# live curl + getaddrinfo + open_public_socket) and asserts:
#   1. a positive HTTPS fetch to a public host succeeds and mints the public
#      media source handle,
#   2. a media source that resolves to a private address is not minted
#      (wildcard skip) and is strictly rejected (concrete pointer),
#   3. an HTTP redirect is rejected (no redirect following),
#   4. an approved origin whose DNS resolves to a private address never
#      establishes a connection (open_public_socket) -- the private listener
#      must stay silent,
#   5. aggregate invariant: zero inbound connections to the private listeners.
import http.server
import json
import os
import ssl
import socket
import struct
import subprocess
import sys
import threading

PUBLIC_IP = "1.1.1.1"
PRIVATE_IP = "10.99.0.1"

NETWORK_CONTRACT = "24d172b971ac832c86d4e28c224bfd7e912aa1ea3de4517aa09af2c02d362a20"
MEDIA_CONTRACT = "f060bfec526a4355bd3431d39310bf96e61907f26be0e0dea6e6e8c2d28a415c"


def text(value):
    encoded = value.encode()
    return struct.pack(">H", len(encoded)) + encoded


def frame(correlation, adapter, contract, operation, scope, payload):
    encoded = json.dumps(payload, separators=(",", ":")).encode()
    body = (text(adapter) + text(contract) + struct.pack(">I", 1) + text(operation)
            + text(scope) + struct.pack(">I", len(encoded)) + encoded)
    return struct.pack(">IBBBBQI", 0x4F505256, 1, 1, 0, 0, correlation, len(body)) + body


def start(path):
    parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    process = subprocess.Popen([path], stdin=child)
    child.close()
    return process, parent


def call(channel, correlation, adapter, contract, operation, scope, payload):
    channel.send(frame(correlation, adapter, contract, operation, scope, payload))
    response = channel.recv(65557)
    magic, version, kind, za, zb, observed, length = struct.unpack(">IBBBBQI", response[:20])
    assert (magic, version, kind, za, zb, observed) == (0x4F505256, 1, 2, 0, 0, correlation)
    assert length == len(response) - 20 and response[20] == 0
    return json.loads(response[21:])


def fetch_request(channel, corr, origin, path, pointers=None, response_type="json"):
    payload = {"method": "GET", "origin": origin, "path": path,
               "responseType": response_type}
    if pointers is not None:
        payload["mediaJsonPointers"] = pointers
    return call(channel, corr, "bounded-network-fetch", NETWORK_CONTRACT,
                "fetch", '{"methods":["GET"],"origins":["' + origin + '"]}', payload)


PRIVATE = []
private_lock = threading.Lock()


class PrivateListener(threading.Thread):
    def __init__(self, ip, port):
        super().__init__(daemon=True)
        self.ip = ip
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((ip, port))
        self.sock.listen(8)

    def run(self):
        while True:
            conn, _ = self.sock.accept()
            with private_lock:
                PRIVATE.append((self.ip, self.port, "connect"))
            try:
                conn.recv(4096)
            except OSError:
                pass
            conn.close()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "http://privatestream.example:8000/")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/stations":
            self._json([{"name": "pub", "url": "http://stream.example:8000/audio"},
                        {"name": "priv", "url": "http://privatestream.example:8000/audio"}])
            return
        if self.path == "/single":
            self._json({"url": "http://privatestream.example:8000/x"})
            return
        self._json([])


class HttpsServer(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        httpd = http.server.HTTPServer((PUBLIC_IP, 443), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain("/root/server.crt", "/root/server.key")
        # SSL-wrap and hand the wrapped socket back to the server so its
        # serve_forever loop uses the TLS socket, not the now-invalid original.
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
        self.httpd = httpd

    def run(self):
        self.httpd.serve_forever()


class HttpMediaServer(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.httpd = http.server.HTTPServer((PUBLIC_IP, 8000), Handler)

    def run(self):
        self.httpd.serve_forever()


RESULTS = []


def record(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + ((" - " + detail) if detail else ""),
          flush=True)


def main():
    provider = sys.argv[1] if len(sys.argv) > 1 else "/usr/bin/netprov"

    # 1. CA + server certificate for app.example that curl will trust.
    if os.path.exists("/root/ca.key"):
        os.remove("/root/ca.key")
    os.system("openssl req -x509 -newkey rsa:2048 -nodes -keyout /root/ca.key "
              "-out /root/ca.crt -subj /CN=t08-ca -days 2 2>/dev/null")
    os.system("openssl req -newkey rsa:2048 -nodes -keyout /root/server.key "
              "-out /root/server.csr -subj /CN=app.example 2>/dev/null")
    with open("/root/ext.cnf", "w") as f:
        f.write("subjectAltName=DNS:app.example,DNS:stream.example\n")
    os.system("openssl x509 -req -in /root/server.csr -CA /root/ca.crt "
              "-CAkey /root/ca.key -CAcreateserial -out /root/server.crt -days 2 "
              "-extfile /root/ext.cnf 2>/dev/null")

    # Bootstrap trust: install the lab CA into the system CA bundle paths the
    # provider's (staged) libcurl may read at its compiled default, and force
    # the env overrides so both the provider and the manual curl agree.
    for p in ("/etc/ssl/certs/ca-certificates.crt",
              "/etc/ssl/certs/ca-bundle.crt",
              "/etc/ca-certificates.crt",
              "/etc/pki/tls/certs/ca-bundle.crt"):
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(open("/root/ca.crt").read())
    os.environ["CURL_CA_BUNDLE"] = "/root/ca.crt"
    os.environ["SSL_CERT_FILE"] = "/root/ca.crt"

    # 2. Private listeners (must stay silent).
    for port in (443, 8000):
        PrivateListener(PRIVATE_IP, port).start()

    # 3. Public HTTPS + HTTP servers.
    HttpsServer().start()
    HttpMediaServer().start()
    import time
    time.sleep(1)
    # 4. Drive the real provider.
    proc, chan = start(provider)

    # 4a. Positive control: approved public HTTPS fetch succeeds; only the
    # public media source is minted (the private wildcard entry is skipped).
    r = fetch_request(chan, 1, "https://app.example", "/stations",
                      pointers=["/*/url"])
    record("positive-fetch-public-origin", r.get("ok") is True and r.get("status") == 200,
           json.dumps(r))
    record("wildcard-skips-private-source", r.get("ok") is True
           and set(r.get("sourceHandles", {}).keys()) == {"/0/url"},
           json.dumps(r.get("sourceHandles")))

    # 4b. Strict rejection: a concrete media pointer resolving to a private
    # address must fail the whole fetch.
    r = fetch_request(chan, 2, "https://app.example", "/single", pointers=["/url"])
    record("concrete-pointer-private-rejected",
           r.get("ok") is False and r.get("error") == "media-source-invalid",
           json.dumps(r))

    # 4c. Redirects are never followed at fetch time.
    r = fetch_request(chan, 3, "https://app.example", "/redirect",
                      response_type="text")
    record("redirect-rejected", r.get("ok") is False
           and r.get("error") == "redirect-rejected", json.dumps(r))

    # 4d. DNS-to-private fetch: approved origin resolves to a private address;
    # open_public_socket must refuse to connect. (Fresh provider so the first
    # one's settled state cannot mask an open connection.)
    chan.close()
    proc.wait(timeout=3)
    p2, c2 = start(provider)
    scope = '{"methods":["GET"],"origins":["https://leak.example"]}'
    r = call(c2, 10, "bounded-network-fetch", NETWORK_CONTRACT, "fetch", scope,
             {"method": "GET", "origin": "https://leak.example", "path": "/x",
              "responseType": "json"})
    record("fetch-dns-to-private-refused",
           r.get("ok") is False and r.get("error") == "network-failed",
           json.dumps(r))
    c2.close()
    p2.wait(timeout=3)

    # 5. Aggregate invariant: the private listeners received zero connections.
    with private_lock:
        conns = list(PRIVATE)
    record("no-private-reach-invariant", len(conns) == 0,
           "private connections: " + str(conns))

    failed = sum(1 for _, ok, _ in RESULTS if not ok)
    print("T08-SUMMARY total={} passed={} failed={}".format(len(RESULTS), len(RESULTS) - failed, failed),
          flush=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
