"""Serve a separate reference compatibility copy and collect real browser evidence.

Original checkout is never changed. The only compatibility change is the removed
Object.observe fallback. Fixture clocks/instrumentation are explicit test-only
probes; ordinary / remains interactive with the original game behavior.
"""
import base64
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import shutil

PROJECT = Path(__file__).resolve().parents[1]
REPO = PROJECT.parents[2]
COPY = REPO/'_workspace/reference_evidence'
OUT = PROJECT/'build/reference/captures'

class Handler(SimpleHTTPRequestHandler):
    def do_POST(self):
        try:
            length = int(self.headers.get('Content-Length','0'))
            if not 0 < length <= 16*1024*1024:
                raise ValueError('payload size')
            data = json.loads(self.rfile.read(length))
            name = data['name']
            if not re.fullmatch(r'[a-z0-9-]{1,80}',name):
                raise ValueError('invalid capture name')
            OUT.mkdir(parents=True,exist_ok=True)
            extension = '.jpg' if data.get('browser') else '.png'
            payload = data['image'].split(',')[-1]
            pixels = base64.b64decode(payload,validate=True)
            if not pixels.startswith((b'\xff\xd8',b'\x89PNG')):
                raise ValueError('invalid image signature')
            (OUT/(name+extension)).write_bytes(pixels)
            data.pop('image')
            data.update(sha256=hashlib.sha256(pixels).hexdigest(),bytes=len(pixels))
            (OUT/(name+extension+'.json')).write_text(json.dumps(data,indent=2),encoding='utf-8')
            self.send_response(200); self.end_headers(); self.wfile.write(b'SAVED')
        except (ValueError,KeyError,TypeError) as error:
            self.send_error(400,str(error))

def main():
    shutil.copytree(REPO/'_workspace/reference_compat',COPY,dirs_exist_ok=True)
    source = (COPY/'index.html').read_text(encoding='utf-8')
    source = source.replace('</body>','<script src="reference_probe.js"></script></body>')
    (COPY/'index.html').write_text(source,encoding='utf-8')
    shutil.copyfile(PROJECT/'tools/reference_probe.js',COPY/'reference_probe.js')
    print('REFERENCE EVIDENCE http://127.0.0.1:8770/ ; original checkout unchanged',flush=True)
    ThreadingHTTPServer(('127.0.0.1',8770),partial(Handler,directory=str(COPY))).serve_forever()

if __name__ == '__main__':
    main()
