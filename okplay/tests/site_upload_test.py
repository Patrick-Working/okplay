#!/usr/bin/env python3
# Tests for server/site_upload.py using a fake in-memory FTP object (no network).
# Version: 2026-11-10
import base64
import ftplib
import os
import re
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "server"))
import site_upload as su  # noqa: E402

RD = "/web/site"


class FakeFTP:
    def __init__(self, files=None, corrupt_upload=False, refuse_rename_over=False):
        self.files = dict(files or {})
        self.dirs = {"/web", RD}
        self.corrupt_upload = corrupt_upload
        self.refuse_rename_over = refuse_rename_over
        self.log = []

    def retrbinary(self, cmd, cb):
        self.log.append(cmd)
        path = cmd[5:]
        if path not in self.files:
            raise ftplib.error_perm("550 no such file")
        cb(self.files[path])

    def storbinary(self, cmd, fp):
        self.log.append(cmd)
        data = fp.read()
        if self.corrupt_upload:
            data = data + b"X"
        self.files[cmd[5:]] = data

    def rename(self, a, b):
        self.log.append("RNFR %s / RNTO %s" % (a, b))
        if self.refuse_rename_over and b in self.files:
            raise ftplib.error_perm("550 exists")
        self.files[b] = self.files.pop(a)

    def delete(self, p):
        self.log.append("DELE " + p)
        if p not in self.files:
            raise ftplib.error_perm("550 no such file")
        del self.files[p]

    def mkd(self, p):
        self.log.append("MKD " + p)
        if p in self.dirs:
            raise ftplib.error_perm("550 exists")
        self.dirs.add(p)

    def pwd(self):
        return "/"

    def nlst(self, d):
        self.log.append("NLST " + d)
        if d not in self.dirs:
            raise ftplib.error_perm("550 no such folder")
        return [f for f in self.files if f.rsplit("/", 1)[0] == d]


class Base(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.tmp = self._td.name
        self.src = os.path.join(self.tmp, "src")
        self.bak = os.path.join(self.tmp, "bak")
        os.makedirs(self.src)
        self.lines = []

    def tearDown(self):
        self._td.cleanup()

    def mk(self, rel, data):
        p = os.path.join(self.src, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def run_tool(self, ftp, go=True, strict=False):
        files = su.collect_files([self.src])
        return su.run(ftp, files, RD, go, self.bak, self.lines.append, strict=strict)

    def text(self):
        return "\n".join(self.lines)


class Tests(Base):
    def test_new_file(self):
        self.mk("a.txt", b"hello")
        f = FakeFTP()
        self.assertEqual(self.run_tool(f), 0)
        self.assertEqual(f.files[RD + "/a.txt"], b"hello")
        self.assertIn("new", self.text())
        self.assertFalse(os.path.exists(self.bak))

    def test_same_skipped(self):
        self.mk("a.txt", b"hello")
        f = FakeFTP({RD + "/a.txt": b"hello"})
        self.assertEqual(self.run_tool(f), 0)
        self.assertFalse(any(c.startswith("STOR") for c in f.log))
        self.assertIn("same", self.text())

    def test_changed_replaced_with_backup(self):
        self.mk("a.txt", b"new")
        f = FakeFTP({RD + "/a.txt": b"old"})
        self.assertEqual(self.run_tool(f), 0)
        self.assertEqual(f.files[RD + "/a.txt"], b"new")
        self.assertFalse(any(k.endswith(su.TMP_SUFFIX) for k in f.files))
        found = [os.path.join(d, n) for d, _, ns in os.walk(self.bak) for n in ns]
        self.assertEqual(len(found), 1)
        with open(found[0], "rb") as fh:
            self.assertEqual(fh.read(), b"old")

    def test_verify_mismatch_fails_real_untouched(self):
        self.mk("a.txt", b"new")
        f = FakeFTP({RD + "/a.txt": b"old"}, corrupt_upload=True)
        self.assertEqual(self.run_tool(f), 1)
        self.assertEqual(f.files[RD + "/a.txt"], b"old")
        self.assertFalse(any(k.endswith(su.TMP_SUFFIX) for k in f.files))
        self.assertIn("FAILED", self.text())

    def test_rename_over_refused_delete_then_rename(self):
        self.mk("a.txt", b"new")
        f = FakeFTP({RD + "/a.txt": b"old"}, refuse_rename_over=True)
        self.assertEqual(self.run_tool(f), 0)
        self.assertEqual(f.files[RD + "/a.txt"], b"new")
        self.assertNotIn("DELE " + RD + "/a.txt", f.log)   # moved aside, never deleted in place
        self.assertIn("refused rename", self.text())
        self.assertEqual(sorted(f.files), [RD + "/a.txt"])   # no temp or aside copy left

    def test_rename_fallback_fails_old_file_back(self):
        self.mk("a.txt", b"new")
        f = FakeFTP({RD + "/a.txt": b"old"}, refuse_rename_over=True)
        real = f.rename
        def rename(a, b):
            if a.endswith(su.TMP_SUFFIX) and b not in f.files:
                f.log.append("RNFR %s / RNTO %s (refused)" % (a, b))
                raise ftplib.error_perm("553 not allowed")
            real(a, b)
        f.rename = rename
        self.assertEqual(self.run_tool(f), 1)
        self.assertEqual(f.files[RD + "/a.txt"], b"old")
        self.assertIn("the old one is back", self.text())

    def test_dry_run_changes_nothing(self):
        self.mk("a.txt", b"new")
        self.mk("b.txt", b"b")
        f = FakeFTP({RD + "/a.txt": b"old"})
        before = dict(f.files)
        self.assertEqual(self.run_tool(f, go=False), 0)
        self.assertEqual(f.files, before)
        self.assertFalse(any(c.split()[0] in ("STOR", "DELE", "MKD") or c.startswith("RNFR")
                             for c in f.log))
        self.assertFalse(os.path.exists(self.bak))
        self.assertIn("changed", self.text())

    def test_offline_no_ftp(self):
        self.mk("a.txt", b"x")
        self.assertEqual(self.run_tool(None, go=False), 0)
        self.assertIn("offline", self.text())

    def test_php_last(self):
        self.mk("okplay.php", b"<?php // v1")
        self.mk("a.zip", b"z")
        self.mk("index.html", b"i")
        f = FakeFTP()
        self.assertEqual(self.run_tool(f), 0)
        stors = [c for c in f.log if c.startswith("STOR")]
        self.assertTrue(stors[-1].endswith("okplay.php" + su.TMP_SUFFIX))
        self.assertEqual(len(stors), 3)

    def test_secret_files_skipped(self):
        for n in ("site.ini", "x.log", "y.bak", "k.pem", "k.key", ".htpasswd"):
            self.mk(n, b"secret")
        self.mk("ok.txt", b"ok")
        f = FakeFTP()
        self.assertEqual(self.run_tool(f), 0)
        self.assertEqual(list(f.files), [RD + "/ok.txt"])
        self.assertEqual(self.text().count("skipped (not uploaded by rule)"), 6)

    def test_password_never_in_output(self):
        self.mk("a.txt", b"new")
        f = FakeFTP({RD + "/a.txt": b"old"}, corrupt_upload=True)
        self.run_tool(f)
        self.assertNotIn("hunter2-secret", self.text())
        # config loading errors must not echo the password either
        cfgp = os.path.join(self.tmp, "c.ini")
        with open(cfgp, "w") as fh:
            fh.write("[site]\nhost=h\nuser=u\npassword=hunter2-secret\ntls=none\n")
        with self.assertRaises(su.UploadError) as cm:
            su.load_config(cfgp)
        self.assertNotIn("hunter2-secret", str(cm.exception))

    def test_subfolder_creation(self):
        self.mk("sub/deep/a.txt", b"hi")
        f = FakeFTP()
        self.assertEqual(self.run_tool(f), 0)
        self.assertEqual(f.files[RD + "/sub/deep/a.txt"], b"hi")
        self.assertIn(RD + "/sub", f.dirs)
        self.assertIn(RD + "/sub/deep", f.dirs)

    def test_version_warning_and_strict(self):
        old = b"<?php\n// Script version: 1\n echo 1;"
        new = b"<?php\n// Script version: 1\n echo 2;"
        self.mk("okplay.php", new)
        f = FakeFTP({RD + "/okplay.php": old})
        self.assertEqual(self.run_tool(f, go=True), 0)
        self.assertIn("version line did not", self.text())
        self.lines.clear()
        f = FakeFTP({RD + "/okplay.php": old})
        self.assertEqual(self.run_tool(f, go=True, strict=True), 1)
        self.assertEqual(f.files[RD + "/okplay.php"], old)

    def test_tls_none_refused_without_allow(self):
        cfgp = os.path.join(self.tmp, "c.ini")
        with open(cfgp, "w") as fh:
            fh.write("[site]\nhost=h\nuser=u\npassword=p\ntls=none\n")
        with self.assertRaises(su.UploadError):
            su.load_config(cfgp)

    # ---- a site saved in FileZilla, the certificate check, --check ----
    def fz(self, sites, certs=""):
        d = os.path.join(self.tmp, "fz")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "sitemanager.xml"), "w") as fh:
            fh.write("<FileZilla3><Servers><Folder>hosting" + sites + "</Folder></Servers></FileZilla3>")
        with open(os.path.join(d, "trustedcerts.xml"), "w") as fh:
            fh.write("<FileZilla3><TrustedCerts>" + certs + "</TrustedCerts></FileZilla3>")
        return d

    @staticmethod
    def site(name, host="ftp.example.org", proto="0", enc="base64", pw="hunter2-secret", port="21"):
        text = base64.b64encode(pw.encode()).decode() if enc == "base64" else pw
        passel = '<Pass encoding="%s">%s</Pass>' % (enc, text) if pw else ""
        return ("<Server><Host>%s</Host><Port>%s</Port><Protocol>%s</Protocol><User>webuser</User>%s"
                "<Logontype>1</Logontype><Name>%s</Name></Server>" % (host, port, proto, passel, name))

    @staticmethod
    def cert(der, host="ftp.example.org", port="21"):
        return "<Certificate><Data>%s</Data><Host>%s</Host><Port>%s</Port></Certificate>" % (der.hex(), host, port)

    def test_filezilla_site_read(self):
        d = self.fz(self.site("other", host="x.example.org") + self.site("My Site"),
                    self.cert(b"cert-one") + self.cert(b"cert-two", port="990") + self.cert(b"cert-three", host="x.example.org"))
        cfg = su.load_config(None, "my site", d)
        self.assertEqual((cfg["host"], cfg["port"], cfg["user"], cfg["password"], cfg["tls"]),
                         ("ftp.example.org", 21, "webuser", "hunter2-secret", "explicit"))
        self.assertEqual(cfg["pins"], {su.sha(b"cert-one")})        # this host and port only
        self.assertNotIn("hunter2-secret", cfg["source"])

    def test_filezilla_site_from_config_wins_over_typed_values(self):
        d = self.fz(self.site("s1"))
        cfgp = os.path.join(self.tmp, "c.ini")
        with open(cfgp, "w") as fh:
            fh.write("[site]\nfilezilla_site=s1\nhost=elsewhere\nuser=u\npassword=p\nremote_dir=/web/site\n")
        cfg = su.load_config(cfgp, None, d)
        self.assertEqual((cfg["host"], cfg["user"], cfg["password"], cfg["remote_dir"]),
                         ("ftp.example.org", "webuser", "hunter2-secret", "/web/site"))

    def test_filezilla_site_refusals(self):
        d = self.fz(self.site("sftp", proto="1") + self.site("locked", enc="crypt") + self.site("nopw", pw="")
                    + self.site("twice") + self.site("twice") + self.site("plain", proto="6"))
        for name, part in (("sftp", "protocol"), ("locked", "master password"), ("nopw", "no saved password"),
                           ("twice", "more than one"), ("absent", "no site"), ("plain", "tls = none refused")):
            with self.assertRaises(su.UploadError) as cm:
                su.load_config(None, name, d)
            self.assertIn(part, str(cm.exception))
            self.assertNotIn("hunter2-secret", str(cm.exception))
        with self.assertRaises(su.UploadError):                     # no FileZilla at all
            su.load_config(None, "sftp", os.path.join(self.tmp, "nowhere"))

    def test_fingerprint_in_config(self):
        cfgp = os.path.join(self.tmp, "c.ini")
        fp = su.sha(b"the-cert")
        with open(cfgp, "w") as fh:
            fh.write("[site]\nhost=h\nuser=u\npassword=p\ntls_fingerprint=SHA256:%s\n"
                     % ":".join(re.findall("..", fp)).upper())
        cfg = su.load_config(cfgp)
        self.assertTrue(su.pinned_ok(b"the-cert", cfg["pins"]))
        self.assertFalse(su.pinned_ok(b"another-cert", cfg["pins"]))
        with open(cfgp, "w") as fh:
            fh.write("[site]\nhost=h\nuser=u\npassword=p\ntls_fingerprint=abcd\n")
        with self.assertRaises(su.UploadError):
            su.load_config(cfgp)

    def test_check_lists_and_changes_nothing(self):
        f = FakeFTP({RD + "/okplay.php": b"x", RD + "/index.html": b"y", "/web/other.txt": b"z"})
        before = dict(f.files)
        self.assertEqual(su.check(f, RD, self.lines.append), 0)
        self.assertIn("  index.html\n  okplay.php", self.text())
        self.assertNotIn("other.txt", self.text())
        self.assertEqual(f.files, before)
        self.assertEqual([c for c in f.log if not c.startswith("NLST")], [])
        self.assertEqual(su.check(f, "/missing", self.lines.append), 1)

    def test_remote_dir_that_is_a_local_path_refused(self):
        self.mk("a.txt", b"x")
        for rd in ("C:/Program Files/Git/web/site", "C:\web", "\web\site"):
            self.assertEqual(su.main([self.src, "--offline", "--remote-dir", rd,
                                      "--config", os.path.join(self.tmp, "none.ini")]), 2)
        self.assertEqual(su.main([self.src, "--offline", "--remote-dir", "/web/site",
                                  "--config", os.path.join(self.tmp, "none.ini")]), 0)

    def test_check_goes_alone(self):
        self.assertEqual(su.main(["--check", "--go"]), 2)
        self.assertEqual(su.main(["--check", self.src]), 2)


if __name__ == "__main__":
    unittest.main(verbosity=1)
