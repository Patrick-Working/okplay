"""Serve a browser game build plus its original data files, for testing Online Play locally.

    python okweb/dplay/test/serve_game.py --build <game>/build_wasm --game <the game's installed folder> [--port 8888]

A path is looked up in the build folder first, then in the game folder - case-insensitively, as the
game's own files are named on Windows (the page asks for data/empires2_x1_p1.dat, the install has
Data/...). Nothing is copied or linked into either folder.
"""
import argparse
import functools
import http.server
import os
import socketserver
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))


def find(root, rel):
    """rel ('data/terrain.drs') under root, matching each part without regard to case"""
    cur = root
    for part in [p for p in rel.split('/') if p]:
        if part in ('.', '..'):
            return None
        exact = os.path.join(cur, part)
        if os.path.exists(exact):
            cur = exact
            continue
        try:
            match = next((n for n in os.listdir(cur) if n.lower() == part.lower()), None)
        except (NotADirectoryError, FileNotFoundError):
            return None
        if match is None:
            return None
        cur = os.path.join(cur, match)
    return cur


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      '.wasm': 'application/wasm', '.js': 'text/javascript', '.mjs': 'text/javascript'}

    def __init__(self, *a, build=None, game=None, **k):
        self.build, self.game = build, game
        super().__init__(*a, directory=build, **k)

    def translate_path(self, path):
        rel = urllib.parse.unquote(urllib.parse.urlparse(path).path).lstrip('/') or 'index.html'
        for root in (self.build, self.game, HERE):   # (HERE: twin.html, the two-player test page)
            hit = find(root, rel)
            if hit and os.path.isfile(hit):
                return hit
        return os.path.join(self.build, rel)

    def end_headers(self):
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('Access-Control-Allow-Origin', '*')
        super().end_headers()

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--port', type=int, default=8888)
    a = ap.parse_args()
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    h = functools.partial(Handler, build=os.path.abspath(a.build), game=os.path.abspath(a.game))
    with socketserver.ThreadingTCPServer(('127.0.0.1', a.port), h) as srv:
        print(f'serving {a.build} (+ {a.game}) on http://127.0.0.1:{a.port}/', flush=True)
        srv.serve_forever()


if __name__ == '__main__':
    main()
