"""Router simulator for okplay NAT-traversal tests (pairs with the OKP_TEST_NAT hook in src/net.c).

    python tests/natsim.py --port 47700 --peer 10.0.1.2=port --peer 10.0.2.2=symmetric

Every okplay process ("peer") has a fake private IPv4 and sits behind its own fake router with a
public IP 203.0.113.<n>. Router types:
  open        no NAT at all (like a public IP, or IPv6): give it a public-looking private IP
  full        endpoint-independent mapping, no filtering        ("full cone")
  restricted  endpoint-independent mapping, filter by remote IP  ("restricted cone")
  port        endpoint-independent mapping, filter by remote IP+port ("port-restricted", most homes)
  symmetric   new public port per destination + IP+port filter  (many phone carriers / CGNAT)
Mapped ports keep the peer's own port when free (port preservation), except symmetric (random).
A STUN server lives at 198.51.100.1:3478. No hairpinning, no relay.
Every drop is logged with its reason; a summary is printed on exit.
"""
import argparse
import random
import signal
import socket
import struct
import sys
import time

STUN_IP, STUN_PORT = "198.51.100.1", 3478
MAGIC = 0x2112A442
TYPES = ("open", "full", "restricted", "port", "symmetric")


def v4_from16(b):
    return socket.inet_ntoa(b[12:16]) if b[:12] == b"\0" * 10 + b"\xff\xff" else None


def to16(ip):
    return b"\0" * 10 + b"\xff\xff" + socket.inet_aton(ip)


class Router:
    def __init__(self, kind, private_ip, public_ip):
        self.kind, self.private_ip = kind, private_ip
        self.public_ip = private_ip if kind == "open" else public_ip
        self.real = None                 # the peer's real 127.0.0.1 socket
        self.eim = {}                    # private port -> public port
        self.edm = {}                    # (private port, dest) -> public port
        self.rev = {}                    # public port -> (private port, dest or None)
        self.sent = {}                   # public port -> set of dests contacted (ip, port)

    def map_out(self, pport, dest):
        """Outbound: which public port does private port pport use toward dest?"""
        if self.kind == "open":
            pub = pport
            self.rev[pub] = (pport, None)
        elif self.kind == "symmetric":
            pub = self.edm.get((pport, dest))
            if pub is None:
                pub = random.randint(20000, 60000)
                while pub in self.rev:
                    pub = random.randint(20000, 60000)
                self.edm[(pport, dest)] = pub
                self.rev[pub] = (pport, dest)
        else:
            pub = self.eim.get(pport)
            if pub is None:
                pub = pport if pport not in self.rev else random.randint(20000, 60000)
                self.eim[pport] = pub
                self.rev[pub] = (pport, None)
        self.sent.setdefault(pub, set()).add(dest)
        return pub

    def admit(self, pub, src):
        """Inbound on public port pub from src=(ip, port): which private port, or (None, reason)."""
        m = self.rev.get(pub)
        if m is None:
            return None, "no mapping on that port"
        pport, dest = m
        sent = self.sent.get(pub, set())
        if self.kind in ("open", "full"):
            return pport, None
        if self.kind == "restricted":
            return (pport, None) if any(d[0] == src[0] for d in sent) else (None, "restricted: never sent to that IP")
        if self.kind == "port":
            return (pport, None) if src in sent else (None, "port-restricted: never sent to that IP:port")
        return (pport, None) if src == dest else (None, "symmetric: mapping belongs to another destination")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=47700)
    ap.add_argument("--peer", action="append", default=[], help="PRIVATE_IP=TYPE")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--secs", type=float, default=600, help="run this long, then print the summary")
    a = ap.parse_args()

    routers = {}
    for i, spec in enumerate(a.peer):
        ip, kind = spec.split("=")
        assert kind in TYPES, kind
        routers[ip] = Router(kind, ip, f"203.0.113.{i + 1}")
    by_public = {r.public_ip: r for r in routers.values()}

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", a.port))
    s.settimeout(0.5)
    stats = {}

    def note(what):
        stats[what] = stats.get(what, 0) + 1
        if not a.quiet and stats[what] <= 3:
            print(f"{time.strftime('%H:%M:%S')} {what}", flush=True)

    def deliver(router, pport, src_ip, src_port, data):
        if router.real is None:
            note("drop: target never registered")
            return
        s.sendto(b"NATR" + to16(src_ip) + struct.pack("!H", src_port) + data, (router.real[0], pport))

    def finish(*_):
        print("SUMMARY " + ", ".join(f"{k}={v}" for k, v in sorted(stats.items())), flush=True)
        sys.exit(0)

    signal.signal(signal.SIGTERM, finish)
    signal.signal(signal.SIGINT, finish)
    print(f"natsim on 127.0.0.1:{a.port}: " + ", ".join(f"{r.private_ip} {r.kind} -> {r.public_ip}" for r in routers.values()),
          flush=True)
    deadline = time.time() + a.secs
    while time.time() < deadline:
        try:
            pkt, real = s.recvfrom(70000)
        except socket.timeout:
            continue
        except ConnectionResetError:
            continue
        if len(pkt) < 26 or pkt[:4] != b"NATS":
            note("drop: not a wrapped packet")
            continue
        priv_ip = socket.inet_ntoa(pkt[4:8])
        dst_ip = v4_from16(pkt[8:24])
        dst_port = struct.unpack("!H", pkt[24:26])[0]
        data = pkt[26:]
        me = routers.get(priv_ip)
        if me is None:
            note("drop: unknown sender")
            continue
        me.real = real
        pport = real[1]
        if dst_ip is None:
            note("drop: IPv6 not simulated")
            continue
        pub = me.map_out(pport, (dst_ip, dst_port))

        if (dst_ip, dst_port) == (STUN_IP, STUN_PORT):
            if len(data) >= 20 and data[:2] == b"\x00\x01":
                xport = pub ^ (MAGIC >> 16)
                xip = struct.unpack("!I", socket.inet_aton(me.public_ip))[0] ^ MAGIC
                attr = struct.pack("!HHBBHI", 0x0020, 8, 0, 1, xport, xip)
                resp = struct.pack("!HHI", 0x0101, len(attr), MAGIC) + data[8:20] + attr
                back, why = me.admit(pub, (STUN_IP, STUN_PORT))
                if back is None:
                    note("drop stun reply: " + why)
                else:
                    deliver(me, back, STUN_IP, STUN_PORT, resp)
                    note("stun answered")
            continue

        target = by_public.get(dst_ip)
        if target is None:
            note("drop: " + ("private address is not routable" if dst_ip.startswith(("10.", "192.168.", "172.")) else "nobody at that address"))
            continue
        if target is me:
            note("drop: no hairpinning")
            continue
        tport, why = target.admit(dst_port, (me.public_ip, pub))
        if tport is None:
            note(f"drop into {target.kind}: {why}")
            continue
        deliver(target, tport, me.public_ip, pub, data)
        note(f"delivered {me.kind}->{target.kind}")
    finish()


if __name__ == "__main__":
    main()
