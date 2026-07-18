#!/usr/bin/env python3
"""Minimal HTTP server with Cross-Origin Isolation headers.

SharedArrayBuffer (used by SDL3/Emscripten) requires:
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp

Usage:
  python scripts/serve-web.py
  python scripts/serve-web.py --entry science_lab.html --no-open
  python scripts/serve-web.py 8080 build/web/wasm/release
"""

import argparse
import http.server
import os
import socketserver
import sys
import threading
import webbrowser

_MIME_OVERRIDES = {
    ".wasm": "application/wasm",
    ".js":   "application/javascript",
    ".html": "text/html",
    ".data": "application/octet-stream",
}


def _normalise_entry(entry):
    entry = (entry or "").strip().replace("\\", "/")
    while entry.startswith("/"):
        entry = entry[1:]
    if not entry:
        entry = "caged_kingdom.html"
    return entry


class COOPCOEPHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            self.send_response(302)
            self.send_header("Location", "/" + self.server.entry_file)
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
    allow_reuse_address = True


def _find_web_dir(repo_root):
    for variant in ("release", "dist"):
        d = os.path.join(repo_root, "build", "web", "wasm", variant)
        if os.path.isdir(d):
            return d
    return None


def _pick_entry(directory, requested):
    if requested:
        entry = _normalise_entry(requested)
        if not os.path.isfile(os.path.join(directory, entry)):
            print(f"ERROR: Entry file not found: {os.path.join(directory, entry)}")
            sys.exit(1)
        return entry

    for candidate in ("caged_kingdom.html", "science_lab.html",
                      "space_demo.html"):
        if os.path.isfile(os.path.join(directory, candidate)):
            return candidate

    html_files = sorted(
        f for f in os.listdir(directory)
        if f.lower().endswith(".html") and os.path.isfile(os.path.join(directory, f)))
    if html_files:
        return html_files[0]

    print(f"ERROR: No .html entry found in {directory}")
    sys.exit(1)


def _parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Serve a JCE WASM build with COOP/COEP headers.")
    parser.add_argument("legacy_port", nargs="?", type=int,
                        help="legacy positional port")
    parser.add_argument("legacy_directory", nargs="?",
                        help="legacy positional web output directory")
    parser.add_argument("--port", type=int, default=None,
                        help="port to listen on (default: 8080)")
    parser.add_argument("--host", default="127.0.0.1",
                        help="host/interface to bind (default: 127.0.0.1)")
    parser.add_argument("--dir", "--directory", dest="directory",
                        help="web output directory")
    parser.add_argument("--entry", help="HTML entry file to open/redirect to")
    parser.add_argument("--no-open", action="store_true",
                        help="do not open the default browser")
    return parser.parse_args(argv)


def main():
    args = _parse_args(sys.argv[1:])
    port = args.port or args.legacy_port or 8080
    directory = args.directory or args.legacy_directory

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

    directory = os.path.abspath(directory)
    entry = _pick_entry(directory, args.entry)

    os.chdir(directory)
    server = _ThreadedHTTPServer((args.host, port), COOPCOEPHandler)
    server.entry_file = entry
    browser_host = "localhost" if args.host in ("", "0.0.0.0", "::") else args.host
    url = f"http://{browser_host}:{port}/{entry}"
    print(f"Serving {directory}")
    print(f"  entry: {entry}")
    print(f"  url:   {url}")
    print(f"  COOP/COEP headers enabled (SharedArrayBuffer available)")
    print(f"  Press Ctrl+C to stop")

    if not args.no_open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
