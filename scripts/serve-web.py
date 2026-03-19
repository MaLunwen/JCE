#!/usr/bin/env python3
"""Minimal HTTP server with Cross-Origin Isolation headers.

SharedArrayBuffer (used by SDL3/Emscripten) requires:
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp

Usage: python serve-web.py [port] [directory]
  Defaults: port=8080, directory=build/web/wasm/src
"""

import http.server
import os
import sys


class COOPCOEPHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        # Redirect bare "/" to JCE.html (Emscripten output name)
        if self.path == "/":
            self.send_response(302)
            self.send_header("Location", "/JCE.html")
            self.end_headers()
            return
        super().do_GET()

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    directory = sys.argv[2] if len(sys.argv) > 2 else None

    # Default to build/web/wasm/src relative to repo root
    if directory is None:
        repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        directory = os.path.join(repo_root, "build", "web", "wasm", "src")

    if not os.path.isdir(directory):
        print(f"ERROR: Directory not found: {directory}")
        print("Run scripts/build-web.bat first.")
        sys.exit(1)

    os.chdir(directory)
    server = http.server.HTTPServer(("", port), COOPCOEPHandler)
    print(f"Serving {directory}")
    print(f"  http://localhost:{port}")
    print(f"  COOP/COEP headers enabled (SharedArrayBuffer available)")
    print(f"  Press Ctrl+C to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
