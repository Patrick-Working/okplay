"""Copy a folder into a machine through okagent (vmctl.py put), big files in pieces.

  python putdir.py AGENT LOCAL_DIR REMOTE_DIR

A 64 MB Windows 98 VM drops the connection on one 20 MB+ put, so files over 2 MB go in 2 MB
pieces, appended one by one with COPY /B. Folders are made as needed.
"""
import ntpath
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PIECE = 2 * 1024 * 1024
agent, local, remote = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3].rstrip('\\')


def vm(*args):
    r = subprocess.run([sys.executable, os.path.join(HERE, 'vmctl.py'), '--agent', agent, *args],
                       capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if not out.startswith('ok'):
        raise SystemExit('%s: %s' % (' '.join(args[:2]), out))
    return out


def put(src, dst):
    size = os.path.getsize(src)
    if size <= PIECE:
        vm('put', src, dst)
        return
    # pieces: the first becomes the file, each next one is appended (COPY /B a+b a) - one short
    # command at a time (Windows 9x command lines stop at 127 characters)
    tmp = os.path.join(os.environ.get('TEMP', '.'), 'putdir_piece.bin')
    done = 0
    with open(src, 'rb') as f:
        i = 0
        while True:
            data = f.read(PIECE)
            if not data:
                break
            with open(tmp, 'wb') as t:
                t.write(data)
            if i == 0:
                vm('put', tmp, dst)
            else:
                part = ntpath.join(ntpath.dirname(dst), 'PIECE.TMP')
                vm('put', tmp, part)
                vm('run', r'C:\COMMAND.COM /c copy /b %s+%s %s' % (dst, part, dst))
                for _ in range(60):   # COPY runs on its own: wait for the new size
                    time.sleep(0.5)
                    if str(done + len(data)) in vm('ls', dst):
                        break
                else:
                    raise SystemExit('%s: piece %d not appended' % (dst, i))
                vm('del', part)
            done += len(data)
            i += 1
    os.remove(tmp)


t0 = time.time()
total = 0
vm('mkdir', remote)
for root, dirs, files in os.walk(local):
    rel = os.path.relpath(root, local)
    rdir = remote if rel == '.' else remote + '\\' + rel.replace('/', '\\').replace(os.sep, '\\')
    if rel != '.':
        vm('mkdir', rdir)
    for name in sorted(files):
        src = os.path.join(root, name)
        put(src, rdir + '\\' + name)
        total += os.path.getsize(src)
        print('put', rdir + '\\' + name, os.path.getsize(src), flush=True)
print('%d bytes in %.0f s' % (total, time.time() - t0))
