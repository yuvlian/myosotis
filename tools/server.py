#!/usr/bin/env python3
"""Logging HTTP server for myosotis development.

Listens on 127.0.0.1:3000 (override with --host/--port) and prints every
request's method, path, and headers, plus the request body for body-bearing
methods (pretty-printed if JSON). Every request gets a 200 with an empty
JSON object back, except /serverinfos.json and /serverinfos_* which return a
canned JSON blob so the Http patch's serverinfos rewrite resolves.

Usage:
    python server.py
    python server.py --port 8080 --host 0.0.0.0
"""
import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


CANNED_SERVERINFOS = json.dumps({
    "serverInfos": [],
    "notice": "",
    "version": "1.0.0",
}).encode("utf-8")


class LoggingHandler(BaseHTTPRequestHandler):
    # The default BaseHTTPRequestHandler stderr access log is noisy and less
    # useful than our own; silence it and print our own richer view.
    def log_message(self, fmt, *args):
        pass

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length) if length > 0 else b""

    def _dump(self, body):
        # Header line: method + path + client.
        print(f"\n--- {self.command} {self.path}  "
              f"from {self.client_address[0]}:{self.client_address[1]}")
        # Headers, sorted for stable reading across requests.
        for k, v in sorted(self.headers.items()):
            print(f"    {k}: {v}")
        if body:
            try:
                pretty = json.dumps(json.loads(body), indent=2, ensure_ascii=False)
                print("    body (json):")
                for line in pretty.splitlines():
                    print(f"      {line}")
            except Exception:
                preview = body[:200]
                print(f"    body (raw, {len(body)} bytes): {preview!r}")
        else:
            print("    body: <empty>")
        sys.stdout.flush()

    def _respond(self):
        # /serverinfos.json is the endpoint the Http patch rewrites to.
        if self.path == "/serverinfos.json" or self.path.startswith("/serverinfos_"):
            self._send(200, CANNED_SERVERINFOS)
            return
        # Default: empty JSON object so the game's parsers have something.
        self._send(200, b"{}")

    def _send(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    # Unified handler for all methods. Reading the body before _dump means a
    # malformed body can't leave the connection half-read for _respond.
    def _handle(self):
        body = self._read_body() if self.command in ("POST", "PUT", "PATCH") else b""
        self._dump(body)
        self._respond()

    def do_GET(self):     self._handle()
    def do_POST(self):    self._handle()
    def do_PUT(self):     self._handle()
    def do_PATCH(self):   self._handle()
    def do_DELETE(self):  self._handle()
    def do_HEAD(self):    self._handle()
    def do_OPTIONS(self): self._handle()


def main():
    ap = argparse.ArgumentParser(description="Logging HTTP server for myosotis dev")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3000)
    args = ap.parse_args()

    srv = ThreadingHTTPServer((args.host, args.port), LoggingHandler)
    print(f"myosotis dev server listening on http://{args.host}:{args.port}")
    print("Ctrl-C to stop.\n")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down.")
        srv.shutdown()


if __name__ == "__main__":
    main()
