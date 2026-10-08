#!/usr/bin/env python3
"""Serves a web build and a disc image with HTTP range requests.

usage: serve.py <build dir> <disc image> [port]
"""

import http.server
import os
import re
import sys

BUILD = os.path.abspath(sys.argv[1])
DISC = os.path.abspath(sys.argv[2])
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8620
TYPES = {'.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.data': 'application/octet-stream'}


class Handler(http.server.BaseHTTPRequestHandler):
    def target(self):
        path = self.path.split('?', 1)[0]
        if path in ('/', '/index.html'):
            path = '/ps2web.html'
        if path == '/disc.iso':
            return DISC
        name = os.path.normpath(path.lstrip('/'))
        if name.startswith('..') or os.sep in name:
            return None
        full = os.path.join(BUILD, name)
        return full if os.path.isfile(full) else None

    def headers_for(self, path, length):
        self.send_header('Content-Type', TYPES.get(os.path.splitext(path)[1], 'application/octet-stream'))
        self.send_header('Content-Length', str(length))
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Cache-Control', 'no-cache')
        self.end_headers()

    def do_HEAD(self):
        path = self.target()
        if not path:
            self.send_error(404)
            return
        self.send_response(200)
        self.headers_for(path, os.path.getsize(path))

    def do_GET(self):
        path = self.target()
        if not path:
            self.send_error(404)
            return
        size = os.path.getsize(path)
        match = re.match(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
        start, end = 0, size - 1
        if match:
            start = int(match.group(1))
            end = min(int(match.group(2)) if match.group(2) else size - 1, size - 1)
            self.send_response(206)
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        else:
            self.send_response(200)
        self.headers_for(path, end - start + 1)
        with open(path, 'rb') as f:
            f.seek(start)
            left = end - start + 1
            while left > 0:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)

    def log_message(self, fmt, *args):
        pass


print('serving %s and %s on http://127.0.0.1:%d/' % (BUILD, DISC, PORT))
http.server.ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
