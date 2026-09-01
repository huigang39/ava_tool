"""Loopback-only cookie-auth fixture for update-download compatibility tests."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body, cookie=None):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        if cookie:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path == "/login" and body == b"username=old-user&password=old-password":
            self.reply(200, b"{}", "session=authorized; Path=/")
        else:
            self.reply(403, b"invalid credentials")

    def do_GET(self):
        if self.headers.get("Cookie") == "session=authorized":
            self.reply(200, b"MZ-cookie-protected-test-installer")
        elif self.path.startswith("/401/"):
            self.reply(401, b"login required")
        else:
            self.reply(200, b'<form><input type="password"></form>')


def start():
    from threading import Thread
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    return server
