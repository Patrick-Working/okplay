#!/usr/bin/env python3
# site_upload.py - safe FTP(S) uploader for the okplay web folder.
#
# What it does: uploads the files of a release folder (or single files) to ONE
# remote folder on the shared web host. Default is a dry run (nothing changes
# on the server). With --go each file is: compared by SHA-256, old copy backed
# up locally, uploaded to "<name>.upload-tmp", read back and verified, renamed
# over the real name, read back again. Nothing else on the server is deleted.
# .php files go last, so pages/zips they refer to are already there.
#
# Usage:
#   python site_upload.py --init-config                 (writes an empty config)
#   python site_upload.py FOLDER                        (dry run, connects)
#   python site_upload.py FOLDER --offline              (dry run, no connection)
#   python site_upload.py FOLDER --go                   (real upload)
#   python site_upload.py FILE1 FILE2 ... [--remote-dir DIR] [--config PATH]
#                         [--backup-dir DIR] [--strict]
#   python site_upload.py --check [--remote-dir DIR]    (log in and list a folder; changes nothing)
#   ... --filezilla-site NAME     host, port, user and password come from the site of that name saved in
#                                 FileZilla's Site Manager, read when the script runs: no second copy of the
#                                 password, and no config file needed (or: filezilla_site = NAME in the config)
# The server's certificate is checked. One that fails the normal check (shared hosts often show the
# hosting company's name) is accepted only if it is the very certificate FileZilla was told to trust for
# that host and port, or the one whose SHA-256 the config names (tls_fingerprint).
# Exit codes: 0 ok / nothing to do, 1 failure, 2 usage or config problem.
# Version: 2026-11-10 (change this line with every change)

import argparse
import configparser
import ftplib
import hashlib
import io
import os
import re
import ssl
import sys
import time
import xml.etree.ElementTree as ET
from base64 import b64decode

VERSION = "2026-11-10"
TMP_SUFFIX = ".upload-tmp"
OLD_SUFFIX = ".upload-old"
SECRET_SUFFIXES = (".ini", ".log", ".bak", ".pem", ".key")
VERSION_RE = re.compile(r"script version:|\bversion\b", re.I)
TEXT_EXTS = (".php", ".html", ".js", ".py")

CONFIG_TEMPLATE = """\
; okplay site_upload config. Keep this file private; never commit it.
[site]
; Either the name of a site saved in FileZilla's Site Manager (host, port, user and
; password are read from it each time; nothing is copied here) ...
filezilla_site =
; ... or the three values themselves:
host =
user =
password =
; default remote folder, e.g. /public_html/aoe2net
remote_dir =
; port = 21
; tls = explicit   (use "none" only together with allow_plain = 1)
; allow_plain = 0
; tls_fingerprint =   (SHA-256 of the one server certificate to accept when the normal check fails)
; backup_dir =
"""


class UploadError(Exception):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def short(h):
    return h[:12] if h else "-"


def default_config_path():
    return os.path.join(os.path.expanduser("~"), ".okplay", "site_upload.ini")


def is_junk(rel):
    base = rel.replace("\\", "/").split("/")[-1].lower()
    return base.startswith(".") or base.endswith(SECRET_SUFFIXES)


def collect_files(paths):
    """Return [(local_path, rel_path)]. rel uses '/'."""
    if len(paths) == 1 and os.path.isdir(paths[0]):
        root = paths[0]
        res = []
        for dp, dns, fns in os.walk(root):
            dns.sort()
            for fn in sorted(fns):
                full = os.path.join(dp, fn)
                if os.path.isfile(full):
                    rel = os.path.relpath(full, root).replace(os.sep, "/")
                    res.append((full, rel))
        return res
    res = []
    for p in paths:
        if not os.path.isfile(p):
            raise UploadError("not a regular file: %s" % p)
        res.append((p, os.path.basename(p)))
    return res


def order_files(files):
    return sorted(files, key=lambda f: (f[1].lower().endswith(".php"), f[1]))


def version_line(data, rel):
    if not rel.lower().endswith(TEXT_EXTS):
        return None
    text = data.decode("utf-8", "replace").splitlines()[:40]
    for line in text:
        if VERSION_RE.search(line):
            return line.strip()
    return None


def remote_join(remote_dir, rel):
    return remote_dir.rstrip("/") + "/" + rel


def fetch(ftp, path):
    """Return bytes of remote file or None if it does not exist."""
    buf = io.BytesIO()
    try:
        ftp.retrbinary("RETR " + path, buf.write)
    except ftplib.error_perm as e:
        if str(e).startswith("550"):
            return None
        raise
    return buf.getvalue()


def ensure_dirs(ftp, remote_dir, rel, out):
    parts = rel.split("/")[:-1]
    cur = remote_dir.rstrip("/")
    for p in parts:
        cur += "/" + p
        try:
            ftp.mkd(cur)
            out("created remote folder %s" % cur)
        except ftplib.error_perm:
            pass  # already exists (a real problem shows up at upload)


def run(ftp, files, remote_dir, go, backup_dir, out, strict=False):
    """files: [(local_path, rel)]. ftp may be None (offline). Returns exit code."""
    good, skipped = [], []
    for lp, rel in files:
        (skipped if is_junk(rel) else good).append((lp, rel))
    for _, rel in skipped:
        out("skipped (not uploaded by rule): %s" % rel)
    good = order_files(good)

    # planning pass
    plan = []
    warnings = 0
    for lp, rel in good:
        with open(lp, "rb") as f:
            data = f.read()
        lh = sha(data)
        rpath = remote_join(remote_dir, rel)
        if ftp is None:
            out("local  %-30s %9d bytes  %s  -> %s" % (rel, len(data), short(lh), rpath))
            plan.append(None)
            continue
        try:
            old = fetch(ftp, rpath)
        except ftplib.all_errors as e:
            out("FAILED reading %s: %s" % (rpath, e))
            return 1
        if old is None:
            status, rh = "new", None
        else:
            rh = sha(old)
            status = "same" if rh == lh else "changed"
        out("%-7s %-30s local %d bytes %s  remote %s %s" % (
            status, rel, len(data), short(lh),
            "%d bytes" % len(old) if old is not None else "-", short(rh)))
        if status == "changed":
            lv, rv = version_line(data, rel), version_line(old, rel)
            if lv is not None and lv == rv:
                warnings += 1
                out("WARNING: %s: content changed but the version line did not" % rel)
        plan.append((data, lh, old, status))

    if ftp is None:
        out("offline dry run: nothing connected, nothing changed")
        return 0
    if strict and warnings:
        out("strict mode: %d version warning(s); nothing uploaded" % warnings)
        return 1
    todo = [(g, p) for g, p in zip(good, plan) if p[3] != "same"]
    if not go:
        out("dry run: %d file(s) would be uploaded, %d unchanged. Use --go to upload."
            % (len(todo), len(good) - len(todo)))
        return 0
    if not todo:
        out("nothing to do")
        return 0

    done = []
    for (lp, rel), (data, lh, old, status) in todo:
        rpath = remote_join(remote_dir, rel)
        tmp = rpath + TMP_SUFFIX
        try:
            if old is not None:
                bp = os.path.join(backup_dir, *rel.split("/"))
                os.makedirs(os.path.dirname(bp), exist_ok=True)
                with open(bp, "wb") as f:
                    f.write(old)
                out("backup of old %s saved to %s" % (rel, bp))
            ensure_dirs(ftp, remote_dir, rel, out)
            ftp.storbinary("STOR " + tmp, io.BytesIO(data))
            back = fetch(ftp, tmp)
            if back is None or sha(back) != lh:
                try:
                    ftp.delete(tmp)
                except ftplib.all_errors:
                    pass
                raise UploadError("verify of temporary upload failed for %s; "
                                  "real file untouched, temp removed" % rel)
            try:
                ftp.rename(tmp, rpath)
            except ftplib.error_perm:
                # The old file is moved aside, not deleted, so there is always
                # a complete copy on the server to put back.
                aside = rpath + OLD_SUFFIX
                out("server refused rename over existing file; "
                    "moving old %s aside, then renaming" % rel)
                try:
                    ftp.delete(aside)       # (left over from an earlier run)
                except ftplib.all_errors:
                    pass
                ftp.rename(rpath, aside)
                try:
                    ftp.rename(tmp, rpath)
                except ftplib.all_errors:
                    ftp.rename(aside, rpath)
                    out("could not put the new %s in place; the old one is back" % rel)
                    raise
                try:
                    ftp.delete(aside)
                except ftplib.all_errors:
                    out("note: %s is left on the server (could not delete it)" % aside)
            fin = fetch(ftp, rpath)
            if fin is None or sha(fin) != lh:
                raise UploadError("final read-back mismatch for %s "
                                  "(old copy is in the backup folder)" % rel)
        except (UploadError,) + tuple(ftplib.all_errors) as e:
            out("FAILED on %s: %s" % (rel, e))
            out("uploaded: %s" % (", ".join(done) or "none"))
            out("not uploaded: %s" % (", ".join(r for (_, r), _p in todo if r not in done) or "none"))
            return 1
        done.append(rel)
        out("uploaded %s (%s, verified)" % (rel, status))
    out("done: %d file(s) uploaded" % len(done))
    return 0


def filezilla_dir():
    return os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "FileZilla")


def filezilla_site(name, fz_dir=None):
    """A site saved in FileZilla's Site Manager, by name (folders do not matter):
    {host, port, user, password, tls, pins}. Read at run time; the password is
    never written anywhere. pins = SHA-256 of the certificates FileZilla was
    told to trust for that host and port."""
    fz_dir = fz_dir or filezilla_dir()
    path = os.path.join(fz_dir, "sitemanager.xml")
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as e:
        raise UploadError("FileZilla's site list could not be read (%s): %s" % (path, e))
    want = name.strip().lower()
    found = [n for n in root.iter("Server")
             if (n.findtext("Name") or "").strip().lower() == want]
    if len(found) != 1:
        raise UploadError("FileZilla has %s site named '%s' (%s)"
                          % ("no" if not found else "more than one", name, path))
    n = found[0]

    def get(tag):
        return (n.findtext(tag) or "").strip()

    proto = get("Protocol") or "0"
    if proto in ("0", "4"):        # FTP (TLS when the server has it) / FTP over explicit TLS
        tls = "explicit"
    elif proto == "6":             # "only use plain FTP (insecure)"
        tls = "none"
    else:                          # 1 = SFTP, 3 = FTP over implicit TLS, ...
        raise UploadError("FileZilla site '%s' uses a protocol this script does not speak "
                          "(%s); it needs FTP or FTP over explicit TLS" % (name, proto))
    pw = n.find("Pass")
    if pw is None or not (pw.text or "").strip():
        raise UploadError("FileZilla site '%s' has no saved password (logon type must be "
                          "'Normal')" % name)
    enc = (pw.get("encoding") or "").lower()
    if enc == "base64":
        password = b64decode(pw.text.strip()).decode("utf-8")
    elif enc == "":
        password = pw.text
    else:
        raise UploadError("FileZilla site '%s': the password is protected by FileZilla's master "
                          "password; put host, user and password in the config instead" % name)
    host, port = get("Host"), int(get("Port") or 21)
    if not host or not get("User"):
        raise UploadError("FileZilla site '%s' has no host or user" % name)
    pins = set()
    try:
        for c in ET.parse(os.path.join(fz_dir, "trustedcerts.xml")).getroot().iter("Certificate"):
            if ((c.findtext("Host") or "").strip().lower() == host.lower()
                    and int(c.findtext("Port") or 0) == port):
                pins.add(sha(bytes.fromhex((c.findtext("Data") or "").strip())))
    except (OSError, ET.ParseError, ValueError):
        pass
    return {"host": host, "port": port, "user": get("User"), "password": password,
            "tls": tls, "pins": pins}


def load_config(path, fz_site=None, fz_dir=None):
    """The [site] section of the config; fz_site (or filezilla_site = in the
    config) fills host, port, user and password from FileZilla's saved site.
    path may be None when fz_site is given."""
    cp = configparser.ConfigParser()
    have = bool(path and cp.read(path, encoding="utf-8")) and "site" in cp
    if not have and not fz_site:
        raise UploadError("config has no [site] section: %s" % path)
    s = cp["site"] if have else {}
    cfg = {k: s.get(k, "").strip() for k in
           ("host", "user", "password", "remote_dir", "port", "tls",
            "allow_plain", "backup_dir", "filezilla_site", "tls_fingerprint")}
    cfg["pins"] = set()
    fz_site = fz_site or cfg["filezilla_site"]
    if fz_site:
        fz = filezilla_site(fz_site, fz_dir)
        cfg["pins"] = fz["pins"]
        for k in ("host", "user", "password", "port"):
            cfg[k] = fz[k]             # the saved site decides: never half one, half the other
        cfg["tls"] = cfg["tls"] or fz["tls"]
        cfg["source"] = "FileZilla's saved site '%s'" % fz_site
    else:
        cfg["source"] = path
    for k in ("host", "user", "password"):
        if not cfg[k]:
            raise UploadError("config value '%s' is empty" % k)
    cfg["port"] = int(cfg["port"] or 21)
    cfg["tls"] = (cfg["tls"] or "explicit").lower()
    if cfg["tls"] == "none":
        if cfg["allow_plain"] != "1":
            raise UploadError("tls = none refused (set allow_plain = 1 to allow)")
    elif cfg["tls"] != "explicit":
        raise UploadError("tls must be 'explicit' or 'none'")
    fp = re.sub(r"[^0-9a-f]", "", cfg["tls_fingerprint"].lower().replace("sha256", ""))
    if fp:
        if len(fp) != 64:
            raise UploadError("tls_fingerprint must be a SHA-256 (64 hex digits)")
        cfg["pins"] = cfg["pins"] | {fp}
    return cfg


class _ReuseTLS(ftplib.FTP_TLS):
    """FTP_TLS whose data connections reuse the control connection's TLS
    session (some servers refuse data connections that do not)."""

    def ntransfercmd(self, cmd, rest=None):
        conn, size = ftplib.FTP.ntransfercmd(self, cmd, rest)
        if self._prot_p:
            conn = self.context.wrap_socket(
                conn, server_hostname=self.host, session=self.sock.session)
        return conn, size


def _tls_session(cfg, verify):
    """Connected and TLS on, not logged in yet."""
    ctx = ssl.create_default_context()
    if not verify:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    ftp = _ReuseTLS(context=ctx, timeout=30)
    try:
        ftp.connect(cfg["host"], cfg["port"])
        ftp.auth()
    except Exception:
        ftp.close()
        raise
    return ftp


def pinned_ok(der, pins):
    """True when the certificate the server showed is one of the trusted ones."""
    return sha(der) in pins


def connect(cfg, out=None):
    """Logged in, binary mode. With TLS the server is known before the
    password goes out: by the normal certificate check, or - when that fails -
    by being exactly a certificate named in cfg["pins"]."""
    if cfg["tls"] != "explicit":
        ftp = ftplib.FTP(timeout=30)
        ftp.connect(cfg["host"], cfg["port"])
    else:
        try:
            ftp = _tls_session(cfg, True)
        except ssl.SSLCertVerificationError as e:
            why = getattr(e, "verify_message", None) or str(e)
            if not cfg.get("pins"):
                raise UploadError(
                    "the server's certificate failed the check (%s) and no trusted certificate "
                    "is known for it: connect once with FileZilla and tick 'always trust', or "
                    "set tls_fingerprint in the config" % why)
            ftp = _tls_session(cfg, False)
            der = ftp.sock.getpeercert(True)
            if not pinned_ok(der, cfg["pins"]):
                ftp.close()
                raise UploadError("the server's certificate failed the check (%s) and is not "
                                  "the trusted one (it is %s...): nothing sent"
                                  % (why, sha(der)[:16]))
            if out:
                out("server certificate: the one already trusted for this host (%s...)"
                    % sha(der)[:16])
    ftp.login(cfg["user"], cfg["password"])
    if cfg["tls"] == "explicit":
        ftp.prot_p()
    ftp.voidcmd("TYPE I")
    return ftp


def check(ftp, remote_dir, out):
    """Read-only look: where the login lands and what a folder holds."""
    out("login folder: %s" % ftp.pwd())
    where = remote_dir or ftp.pwd()
    try:
        names = sorted(ftp.nlst(where))
    except ftplib.all_errors as e:
        out("could not list %s: %s" % (where, e))
        return 1
    names = [n.replace("\\", "/").rstrip("/").split("/")[-1] for n in names]
    names = [n for n in names if n not in (".", "..", "")]
    out("%s holds %d entries:" % (where, len(names)))
    for n in names:
        out("  %s" % n)
    out("check done: nothing was changed")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Safe uploader for the okplay web folder.")
    ap.add_argument("paths", nargs="*", help="one folder, or one or more files")
    ap.add_argument("--remote-dir")
    ap.add_argument("--config", default=default_config_path())
    ap.add_argument("--go", action="store_true", help="really upload (default: dry run)")
    ap.add_argument("--offline", action="store_true", help="dry run without connecting")
    ap.add_argument("--backup-dir")
    ap.add_argument("--strict", action="store_true",
                    help="version-line warnings become errors")
    ap.add_argument("--init-config", action="store_true")
    ap.add_argument("--check", action="store_true",
                    help="log in and list the remote folder; changes nothing")
    ap.add_argument("--filezilla-site", metavar="NAME",
                    help="take host, port, user and password from this site saved in FileZilla")
    ap.add_argument("--filezilla-dir", help=argparse.SUPPRESS)   # (tests)
    ap.add_argument("--version", action="version", version="site_upload " + VERSION)
    try:
        a = ap.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0

    if a.init_config:
        if os.path.exists(a.config):
            print("config already exists, not overwriting: %s" % a.config)
            return 2
        os.makedirs(os.path.dirname(os.path.abspath(a.config)), exist_ok=True)
        with open(a.config, "w", encoding="utf-8") as f:
            f.write(CONFIG_TEMPLATE)
        print("empty config written: %s (fill in host, user, password, remote_dir)" % a.config)
        return 0
    if a.check and (a.go or a.offline or a.paths):
        print("usage problem: --check goes alone (with --remote-dir, --config, --filezilla-site)")
        return 2
    if not a.paths and not a.check:
        print("usage problem: give a folder or files (see --help)")
        return 2
    if a.go and a.offline:
        print("usage problem: --go and --offline cannot be combined")
        return 2
    files = []
    if not a.check:
        try:
            files = collect_files(a.paths)
        except UploadError as e:
            print("usage problem: %s" % e)
            return 2
        if not files:
            print("no files found")
            return 2

    have_cfg = os.path.exists(a.config)
    if a.offline and not have_cfg:
        cfg = None
    elif not have_cfg and not a.filezilla_site:
        print("config not found: %s" % a.config)
        print("Run with --init-config to create an empty one, then fill it in "
              "(or use --filezilla-site NAME).")
        return 2
    else:
        try:
            cfg = load_config(a.config if have_cfg else None, a.filezilla_site, a.filezilla_dir)
        except (UploadError, configparser.Error, ValueError) as e:
            print("config problem: %s" % e)
            return 2
    remote_dir = a.remote_dir or (cfg and cfg["remote_dir"])
    if not remote_dir and not a.check:
        print("no remote dir: use --remote-dir or set remote_dir in the config")
        return 2
    if remote_dir and (re.match(r"[A-Za-z]:", remote_dir) or "\\" in remote_dir):
        # (Git Bash rewrites an argument like /web into C:/Program Files/Git/web)
        print("remote dir looks like a path on this PC, not on the server: %s" % remote_dir)
        print("In Git Bash write it with two slashes (//web/site) or set remote_dir in the config.")
        return 2
    backup_base = a.backup_dir or (cfg and cfg["backup_dir"]) or os.path.join(
        os.path.dirname(os.path.abspath(a.config)), "site_upload_backup")
    backup_dir = os.path.join(backup_base, time.strftime("%Y%m%d-%H%M%S"))

    ftp = None
    if not a.offline:
        try:
            ftp = connect(cfg, print)
        except Exception as e:
            msg = str(e)
            if cfg["password"]:
                msg = msg.replace(cfg["password"], "***")
            print("connection/login failed for %s (login from %s): %s" % (cfg["host"], cfg["source"], msg))
            return 1
        print("connected to %s (login from %s)" % (cfg["host"], cfg["source"]))
    try:
        if a.check:
            return check(ftp, remote_dir, print)
        return run(ftp, files, remote_dir, a.go, backup_dir, print, strict=a.strict)
    finally:
        if ftp is not None:
            try:
                ftp.quit()
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
