#!/usr/bin/env python3
"""Minimal HTTP server with Cross-Origin Isolation headers.

SharedArrayBuffer (used by SDL3/Emscripten) requires:
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp

Usage: python serve-web.py [port] [directory]
  Defaults: port=8080, directory=build/web/wasm/{release|dist} (auto-detected)
"""

import http.server
import os
import sys
import socketserver
import threading
import webbrowser

_MIME_OVERRIDES = {
    ".wasm": "application/wasm",
    ".js":   "application/javascript",
    ".html": "text/html",
    ".data": "application/octet-stream",
}


class COOPCOEPHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            self.send_response(302)
            self.send_header("Location", "/caged_kingdom.html")
            self.end_headers()
            return
        super().do_GET()

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

    def guess_type(self, path):
        ext = os.path.splitext(path)[1].lower()
        return _MIME_OVERRIDES.get(ext, super().guess_type(path))

    def log_message(self, fmt, *args):
        # Suppress 304 Not Modified noise; log everything else
        if len(args) >= 2 and args[1] == "304":
            return
        super().log_message(fmt, *args)


class _ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    """Handle each request in its own thread (browser loads many assets at once)."""
    daemon_threads = True


def _find_web_dir(repo_root):
    for variant in ("release", "dist"):
        d = os.path.join(repo_root, "build", "web", "wasm", variant)
        if os.path.isdir(d):
            return d
    return None


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    directory = sys.argv[2] if len(sys.argv) > 2 else None

    if directory is None:
        repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        directory = _find_web_dir(repo_root)
        if directory is None:
            print("ERROR: No web build found (tried release/ and dist/).")
            print("Run scripts/build-web.bat first.")
            sys.exit(1)

    if not os.path.isdir(directory):
        print(f"ERROR: Directory not found: {directory}")
        sys.exit(1)

    os.chdir(directory)
    server = _ThreadedHTTPServer(("", port), COOPCOEPHandler)
    url = f"http://localhost:{port}"
    print(f"Serving {directory}")
    print(f"  {url}")
    print(f"  COOP/COEP headers enabled (SharedArrayBuffer available)")
    print(f"  Press Ctrl+C to stop")

    threading.Timer(0.4, lambda: webbrowser.open(url)).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
