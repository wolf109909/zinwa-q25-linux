#!/usr/bin/env python3
"""q25-imsd: minimal IMS client for SMS over IP on the Zinwa Q25 (MT6789 modem).

Talks to the modem through the Q25 ModemManager plugin bridge
(/run/q25-ims-bridge.sock), brings up the IMS PDN, registers to the operator IMS core with
IMS-AKA + IPsec (sec-agree), receives SMS (SIP MESSAGE / RP-DATA) and sends SMS.

Standards: 3GPP TS 24.229 (SIP), 24.341 (SMS over IP), 24.011 (RP), 23.040 (TPDU),
TS 33.203 (IMS security), RFC 3310 (Digest AKA), RFC 3329 (sec-agree).

Usage:
  q25-imsd.py run                 # daemon
  q25-imsd.py send <number> <txt> # via control socket
  q25-imsd.py status
"""
import base64
import hashlib
import json
import queue
import os
import pwd
import random
import signal
import re
import select
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smspdu  # noqa: E402

CTL = "/run/q25-imsd.sock"
STATE_DIR = "/var/lib/q25-imsd"
LOG_PREFIX = "q25-imsd"
RT_TABLE = 1403
PDN_STATE = "/run/q25-imsd.pdn"   # modem +EIF id of our IMS bearer (tmpfs: gone at boot)
XFRM_STATE = "/run/q25-imsd.xfrm"  # SAs/policies we installed, removed by the next instance
SMS_LOG_MAX = 1 << 20


def log(*a):
    print(f"{time.strftime('%H:%M:%S')} {LOG_PREFIX}:", *a, flush=True)


def sh(*cmd, check=True):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f"{' '.join(cmd)}: {r.stderr.strip()}")
    return r.stdout


def ip_batch(lines, check=True):
    """Run ip(8) commands from stdin, so keys never appear in /proc/*/cmdline or logs."""
    r = subprocess.run(["ip", "-force", "-batch", "-"], input="\n".join(lines) + "\n",
                       capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f"ip -batch failed ({len(lines)} commands): "
                           + re.sub(r"0x[0-9a-fA-F]{16,}", "<key>", r.stderr.strip()))


def desktop_user():
    """The phone's interactive user: owner of the active graphical session, else UID 1000."""
    try:
        for d in os.listdir("/run/user"):
            if d.isdigit() and int(d) >= 1000:
                return pwd.getpwuid(int(d))
    except (OSError, KeyError):
        pass
    try:
        return pwd.getpwuid(1000)
    except KeyError:
        return None


# ---------------------------------------------------------------- modem (AT)
BRIDGE = "/run/q25-ims-bridge.sock"


class Modem:
    """AT + URC access through the Q25 ModemManager plugin bridge."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(BRIDGE)
        self.wlock = threading.Lock()
        self.next_id = 0
        self.waiters = {}            # id -> [Event, (ok, text)]
        self.urc_handlers = []
        self.mo_handler = None
        self.mtres = queue.Queue()
        self.alive = True
        threading.Thread(target=self._reader, daemon=True).start()

    def send_line(self, line):
        with self.wlock:
            self.sock.sendall(line.encode() + b"\n")

    def _reader(self):
        try:
            self._read_lines()
        except Exception as e:
            log("bridge reader error:", repr(e))
        self.alive = False
        log("ModemManager bridge closed; exiting for a clean restart")
        os._exit(3)

    def _read_lines(self):
        f = self.sock.makefile("r", encoding="utf-8", errors="replace", newline="\n")
        for line in f:
            line = line.rstrip("\n")
            kind, _, rest = line.partition(" ")
            if kind == "ATRES":
                rid, _, rest = rest.partition(" ")
                status, _, text = rest.partition(" ")
                w = self.waiters.get(rid)
                if w:
                    w[1] = (status == "OK", text.replace("\\n", "\n"))
                    w[0].set()
            elif kind == "URC":
                for fn in self.urc_handlers:
                    try:
                        fn(rest)
                    except Exception as e:
                        log("URC handler error:", repr(e))
            elif kind == "MO" and self.mo_handler:
                rid, _, hexpdu = rest.partition(" ")
                try:
                    pdu = bytes.fromhex(hexpdu)
                except ValueError:
                    self.send_line(f"MORES {rid} ERR bad PDU")
                    continue
                self.mo_handler(rid, pdu)
            elif kind == "MTRES":
                self.mtres.put(rest)

    def at(self, cmd, timeout=90):
        self.next_id += 1
        rid = str(self.next_id)
        w = [threading.Event(), None]
        self.waiters[rid] = w
        self.send_line(f"AT {rid} {cmd}")
        if not w[0].wait(timeout + 5):
            self.waiters.pop(rid, None)
            raise RuntimeError(f"{cmd}: no answer from ModemManager")
        self.waiters.pop(rid, None)
        ok, text = w[1]
        if not ok:
            raise RuntimeError(f"{cmd}: {text}")
        return text

    def imsi(self):
        return re.search(r"\d{15}", self.at("AT+CIMI")).group(0)

    def imei(self):
        return re.search(r"\d{15}", self.at("AT+CGSN")).group(0)

    def smsc(self):
        m = re.search(r'\+CSCA:\s*"([^"]+)"', self.at("AT+CSCA?"))
        num = m.group(1)
        return num if num.startswith("+") else "+" + num

    def aka(self, rand, autn):
        apdu = "0088008122" + "10" + rand.hex().upper() + "10" + autn.hex().upper()
        resp = self._csim(apdu)
        if resp[-4:].startswith("61"):
            resp = self._csim("00C00000" + resp[-2:])
        sw, data = resp[-4:], bytes.fromhex(resp[:-4])
        if data[:1] == b"\xdb":
            i, out = 1, []
            for _ in range(3):
                n = data[i]
                out.append(data[i + 1:i + 1 + n])
                i += 1 + n
            return {"res": out[0], "ck": out[1], "ik": out[2]}
        if data[:1] == b"\xdc":
            return {"auts": data[2:2 + data[1]]}
        raise RuntimeError(f"AUTHENTICATE failed: SW {sw}")

    def _csim(self, apdu):
        r = self.at(f'AT+CSIM={len(apdu)},"{apdu}"')
        return r.split('"')[1]


# ---------------------------------------------------------------- IMS PDN
def dec_ip6(dotted):
    b = [int(x) for x in dotted.split(".")]
    return socket.inet_ntop(socket.AF_INET6, bytes(b)) if len(b) == 16 else dotted


class Pdn:
    def __init__(self, modem):
        self.m = modem
        self.ifname = self.addr = None
        self.pcscf = []
        self.eif = queue.Queue()
        self.eif_id = None
        self.on_lost = None          # callback when our IMS bearer goes down
        modem.urc_handlers.append(self._on_urc)

    def _on_urc(self, line):
        m = re.search(r"\+EIF: (\d+), (ifup|ifdown)(?:, (\d+))?", line)
        if not m:
            return
        eid, ev = int(m.group(1)), m.group(2)
        self.eif.put((eid, ev, m.group(3)))
        if ev == "ifdown" and self.eif_id is not None and eid == self.eif_id:
            log(f"IMS bearer {self.ifname} went down")
            self.invalidate()
            if self.on_lost:
                self.on_lost()

    def invalidate(self):
        self.ifname = self.addr = None
        self.eif_id = None
        try:
            os.unlink(PDN_STATE)
        except OSError:
            pass

    def contrdp(self):
        for line in self.m.at("AT+CGCONTRDP").split("+CGCONTRDP:"):
            f = [x.strip().strip('"') for x in line.split(",")]
            if len(f) > 8 and f[2].lower().startswith("ims."):
                return [dec_ip6(x) for x in f[7:9] if x]
        return []

    def up(self):
        self.pcscf = self.contrdp()
        if not self.pcscf:
            log("bringing up IMS PDN")
            self.m.at("AT+EIMSCFG=1,0,0,0,1,1")
            self.m.at("AT+EIMS=1")
            # the "ims" APN entry itself is written by the mtk-ccci plugin (configure_apn)
            # during modem enable; writing it here as well raced with that (CME 5843)
            while not self.eif.empty():
                self.eif.get_nowait()
            self.m.at('AT+EAPNACT=1,"ims","ims",0', timeout=90)
            eif = None
            end = time.time() + 20
            while time.time() < end:
                try:
                    i, ev, typ = self.eif.get(timeout=1)
                except queue.Empty:
                    continue
                if ev == "ifup" and typ == "2":
                    eif = i
                    break
            if eif is None:
                raise RuntimeError("no +EIF for the IMS PDN")
            self.m.at(f'AT+EIF={eif},"ifst","up",3')
            self.eif_id = eif
            self.ifname = f"ccmni{eif % 100}"
            with open(PDN_STATE, "w") as f:
                f.write(str(eif))
            self.pcscf = self.contrdp()
        if not self.ifname:
            self.ifname = self._find_if()
        sh("ip", "link", "set", self.ifname, "up")
        for _ in range(30):
            a = self.global_addrs()
            if a:
                self.addr = a[0]
                break
            time.sleep(1)
        if not self.addr or not self.pcscf:
            raise RuntimeError(f"IMS PDN incomplete: if={self.ifname} addr={self.addr} pcscf={self.pcscf}")
        # traffic from the IMS address must leave via the IMS PDN (Wi-Fi IPv6 would win otherwise)
        sh("ip", "-6", "route", "replace", "default", "via", "fe80::1", "dev", self.ifname, "table", str(RT_TABLE))
        sh("ip", "-6", "rule", "del", "priority", str(RT_TABLE), check=False)
        sh("ip", "-6", "rule", "add", "from", self.addr, "table", str(RT_TABLE), "priority", str(RT_TABLE))
        log(f"IMS PDN on {self.ifname}, P-CSCF {self.pcscf}")

    def global_addrs(self):
        if not self.ifname:
            return []
        out = sh("ip", "-6", "addr", "show", "dev", self.ifname, check=False)
        return re.findall(r"inet6 (2[0-9a-f:]+)/\d+ scope global", out)

    def _find_if(self):
        """IMS bearer already up (daemon restart): use the interface we recorded."""
        try:
            self.eif_id = int(open(PDN_STATE).read().strip())
            return f"ccmni{self.eif_id % 100}"
        except (OSError, ValueError):
            raise RuntimeError("IMS bearer is up but its interface is unknown; "
                               "restart ModemManager to reset the bearer")


# ---------------------------------------------------------------- IPsec
class IPsec:
    """ESP transport SAs for sec-agree. Only touches the SAs/policies it installed
    (recorded in XFRM_STATE so a restarted daemon can clean up its predecessor)."""

    def __init__(self):
        self.active = []
        self._cleanup_previous()

    @staticmethod
    def _del_cmds(handle):
        sas, pols = handle
        cmds = []
        for src, sp, dst, dp, d, _ in pols:
            for proto in ("udp", "tcp"):
                cmds.append(f"xfrm policy del src {src} dst {dst} proto {proto} "
                            f"sport {sp} dport {dp} dir {d}")
        for src, dst, spi in sas:
            cmds.append(f"xfrm state del src {src} dst {dst} proto esp spi {spi}")
        return cmds

    def _save(self):
        try:
            with open(XFRM_STATE + ".tmp", "w") as f:
                json.dump(self.active, f)
            os.replace(XFRM_STATE + ".tmp", XFRM_STATE)
        except OSError as e:
            log("cannot record xfrm state:", e)

    def _cleanup_previous(self):
        try:
            old = json.load(open(XFRM_STATE))
        except (OSError, ValueError):
            return
        cmds = [c for h in old for c in self._del_cmds(h)]
        if cmds:
            ip_batch(cmds, check=False)
        try:
            os.unlink(XFRM_STATE)
        except OSError:
            pass

    def setup(self, ue, pc, ue_c, ue_s, pc_c, pc_s, spi_uc, spi_us, spi_pc, spi_ps, ik):
        key = "0x" + (ik + b"\x00" * 4).hex()
        sas = [  # (src, dst, spi)
            (ue, pc, spi_ps),   # UE port-c -> P-CSCF port-s
            (ue, pc, spi_pc),   # UE port-s -> P-CSCF port-c
            (pc, ue, spi_us),   # P-CSCF port-c -> UE port-s
            (pc, ue, spi_uc),   # P-CSCF port-s -> UE port-c
        ]
        pols = [  # (src, sport, dst, dport, dir, spi)
            (ue, ue_c, pc, pc_s, "out", spi_ps),
            (ue, ue_s, pc, pc_c, "out", spi_pc),
            (pc, pc_c, ue, ue_s, "in", spi_us),
            (pc, pc_s, ue, ue_c, "in", spi_uc),
        ]
        handle = [[list(x) for x in sas], [list(x) for x in pols]]
        # record first, so a crash half-way still gets cleaned up by the next instance
        self.active.append(handle)
        self._save()
        cmds = [f"xfrm state add src {src} dst {dst} proto esp spi {spi} mode transport "
                f"auth-trunc hmac(sha1) {key} 96 enc cipher_null \"\"" for src, dst, spi in sas]
        for src, sp, dst, dp, d, spi in pols:
            for proto in ("udp", "tcp"):
                cmds.append(f"xfrm policy add src {src} dst {dst} proto {proto} sport {sp} dport {dp} "
                            f"dir {d} tmpl src {src} dst {dst} proto esp spi {spi} mode transport")
        try:
            ip_batch(cmds)
        except Exception:
            self.remove(handle)
            raise
        return handle

    def remove(self, handle):
        if handle in self.active:
            self.active.remove(handle)
        ip_batch(self._del_cmds(handle), check=False)
        self._save()

    def flush_old(self, keep=1):
        while len(self.active) > keep:
            self.remove(self.active[0])

    def flush_all(self):
        self.flush_old(keep=0)


# ---------------------------------------------------------------- SIP helpers
def parse_sip(data):
    text = data.decode("utf-8", errors="replace") if isinstance(data, bytes) else data
    head, _, _ = text.partition("\r\n\r\n")
    raw = data if isinstance(data, bytes) else data.encode()
    body = raw[raw.find(b"\r\n\r\n") + 4:]
    lines = head.split("\r\n")
    start = lines[0]
    hdrs = []
    for ln in lines[1:]:
        if ln[:1] in (" ", "\t") and hdrs:
            hdrs[-1] = (hdrs[-1][0], hdrs[-1][1] + " " + ln.strip())
        elif ":" in ln:
            k, v = ln.split(":", 1)
            hdrs.append((k.strip().lower(), v.strip()))
    short = {"v": "via", "f": "from", "t": "to", "i": "call-id", "m": "contact", "l": "content-length",
             "c": "content-type"}
    hdrs = [(short.get(k, k), v) for k, v in hdrs]
    msg = {"start": start, "hdrs": hdrs, "body": body}
    if start.startswith("SIP/2.0"):
        msg["code"] = int(start.split()[1])
    else:
        msg["method"] = start.split()[0]
    return msg


def h(msg, name):
    for k, v in msg["hdrs"]:
        if k == name:
            return v
    return None


def hall(msg, name):
    return [v for k, v in msg["hdrs"] if k == name]


def branch():
    return "z9hG4bK%016x" % random.getrandbits(64)


def tag():
    return "%08x" % random.getrandbits(32)


# ---------------------------------------------------------------- IMS client
class Ims:
    def __init__(self):
        self.m = Modem()
        self.pdn = Pdn(self.m)
        self.ipsec = IPsec()
        self.imsi = self.m.imsi()
        self.imei = self.m.imei()
        mcc, mnc = self.imsi[:3], self.imsi[3:5].zfill(3)
        self.domain = f"ims.mnc{mnc}.mcc{mcc}.3gppnetwork.org"
        self.impi = f"{self.imsi}@{self.domain}"
        self.impu = f"sip:{self.imsi}@{self.domain}"
        self.instance = f"urn:gsma:imei:{self.imei[:8]}-{self.imei[8:14]}-0"
        self.smsc = self.m.smsc()
        self.callid = "%016x@q25" % random.getrandbits(64)
        self.cseq = 0
        self.ports = [(5100, 5101), (5102, 5103)]
        self.pi = 0
        self.registered = False
        self.expires = 600000
        self.assoc = []
        self.service_route = []
        self.mr = random.randint(0, 255)
        self.pending = {}           # branch -> threading.Event/response
        self.sock_c = self.sock_s = None
        self.socks = []              # sockets of the active registration
        self.extra_socks = []        # sockets of a registration in progress
        self.wake = threading.Event()
        self.concat = {}
        self.work = queue.Queue()
        self.mo_wait = {}            # RP-MR -> [Event, (ok, cause)]
        self.mr_lock = threading.Lock()
        self.seen = {}               # (call-id, cseq) -> time, to drop retransmissions
        self.peers = {}              # socket -> {(addr, port)} it may hear from (anti-spoofing)
        self.tcp_slots = threading.BoundedSemaphore(8)
        self.last_reg = None         # (pcscf, sock, ue_c, ue_s, pc_s, sec_client, auth, sec_verify)
        os.makedirs(STATE_DIR, mode=0o700, exist_ok=True)

    # -------- transport
    def _new_sockets(self, pc, ps):
        c = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        c.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        c.bind((self.pdn.addr, pc))
        sv = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        sv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sv.bind((self.pdn.addr, ps))
        t = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        t.bind((self.pdn.addr, ps))
        t.listen(4)
        return [c, sv, t]

    def _close(self, socks):
        for x in socks or []:
            self.peers.pop(x, None)
            try:
                x.close()
            except OSError:
                pass

    def send_request(self, raw, dest, timeout=10, sock=None):
        b = re.search(r"branch=([^;\r\n]+)", raw).group(1)
        ev = threading.Event()
        self.pending[b] = [ev, None]
        (sock or self.sock_c).sendto(raw.encode() if isinstance(raw, str) else raw, dest)
        final = None
        end = time.time() + timeout
        while time.time() < end:
            ev.wait(max(0.0, end - time.time()))
            ev.clear()
            r = (self.pending.get(b) or [None, None])[1]
            if r and r["code"] >= 200:
                final = r
                break
        self.pending.pop(b, None)
        return final

    def _dispatch(self, data, send, requests_ok=True):
        if not data.strip():
            return
        try:
            msg = parse_sip(data)
        except Exception as e:
            log("unparsable SIP:", e)
            return
        if "code" in msg:
            b = re.search(r"branch=([^;\s,]+)", h(msg, "via") or "")
            w = self.pending.get(b.group(1)) if b else None
            if w:
                w[1] = msg
                w[0].set()
        elif not requests_ok:
            log(f"dropping {msg['method']} that did not arrive on a protected port")
        else:
            try:
                self.on_request(msg, send)
            except Exception as e:
                log("request handling error:", repr(e))

    def _tcp_conn(self, conn):
        """SIP over TCP (large messages): frame by Content-Length."""
        buf = b""
        conn.settimeout(600)
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    return
                buf += chunk
                if len(buf) > 1 << 20:
                    log("TCP: oversized SIP stream, closing")
                    return
                while b"\r\n\r\n" in buf:
                    head, _, rest = buf.partition(b"\r\n\r\n")
                    m = re.search(rb"(?im)^(?:content-length|l)\s*:\s*(\d+)", head)
                    n = int(m.group(1)) if m else 0
                    if len(rest) < n:
                        break
                    self._dispatch(head + b"\r\n\r\n" + rest[:n], conn.sendall)
                    buf = rest[n:]
        except OSError:
            pass
        finally:
            conn.close()
            self.tcp_slots.release()

    def _peer_ok(self, sock, addr):
        """Only the P-CSCF may talk to us. Its protected ports are additionally forced
        through ESP by the xfrm "in" policies, so a spoofed source address does not help."""
        ok = (addr[0], addr[1]) in self.peers.get(sock, ())
        if not ok:
            log(f"dropping packet from unexpected peer [{addr[0]}]:{addr[1]}")
        return ok

    def rx_loop(self):
        while True:
            socks = [x for x in (self.socks or []) + (self.extra_socks or []) if x.fileno() >= 0]
            try:
                rd, _, _ = select.select(socks, [], [], 1.0)
            except (OSError, ValueError):
                time.sleep(0.2)
                continue
            for x in rd:
                try:
                    if x.type == socket.SOCK_STREAM:
                        conn, addr = x.accept()
                        if not self._peer_ok(x, addr) or not self.tcp_slots.acquire(blocking=False):
                            conn.close()
                            continue
                        threading.Thread(target=self._tcp_conn, args=(conn,), daemon=True).start()
                        continue
                    data, addr = x.recvfrom(65535)
                except OSError:
                    continue
                if not self._peer_ok(x, addr):
                    continue
                # requests (MESSAGE etc.) are only taken from the IPsec-protected P-CSCF ports
                self._dispatch(data, lambda b, x=x, addr=addr: x.sendto(b, addr),
                               requests_ok=addr[1] != 5060)

    def worker_loop(self):
        while True:
            fn, args = self.work.get()
            try:
                fn(*args)
            except Exception as e:
                log("worker error:", repr(e))

    # -------- registration
    def register(self):
        self.pdn.up()
        last = None
        for pcscf in self.pdn.pcscf:
            if not self.pdn.addr:
                raise RuntimeError("IMS bearer lost during registration")
            try:
                self._register_via(pcscf)
                return
            except Exception as e:
                last = e
                log(f"P-CSCF {pcscf}: {e!r}")
        raise last or RuntimeError("no P-CSCF")

    def _register_via(self, pcscf):
        ue_c, ue_s = self.ports[self.pi]
        new = self._new_sockets(ue_c, ue_s)
        self.extra_socks = new
        sock_c = new[0]
        self.peers[sock_c] = {(pcscf, 5060)}       # unprotected 401 only
        handle = None
        try:
            spi_uc, spi_us = random.randint(0x10000, 0x7fffffff), random.randint(0x10000, 0x7fffffff)
            sec_client = (f"ipsec-3gpp;alg=hmac-sha-1-96;ealg=null;spi-c={spi_uc};spi-s={spi_us};"
                          f"port-c={ue_c};port-s={ue_s}")
            uri = f"sip:{self.domain}"
            auth = None
            for _attempt in range(3):           # 401 -> (AUTS resync -> 401) -> AKA
                r = self.send_request(self._register_msg(ue_c, ue_s, sec_client, auth=auth),
                                      (pcscf, 5060), sock=sock_c)
                if not r or r["code"] != 401:
                    raise RuntimeError(f"initial REGISTER: {r and r['start']}")
                www = h(r, "www-authenticate")
                nonce = re.search(r'nonce="([^"]+)"', www).group(1)
                realm = re.search(r'realm="([^"]+)"', www).group(1)
                raw = base64.b64decode(nonce)
                aka = self.m.aka(raw[:16], raw[16:32])
                if "auts" not in aka:
                    break
                # RFC 3310 3.4: resynchronise with an empty password and the AUTS
                log("AKA sequence out of sync: sending AUTS to resynchronise")
                resp = digest_response(self.impi, realm, nonce, b"", "REGISTER", uri)
                auth = (f'Digest username="{self.impi}",realm="{realm}",nonce="{nonce}",uri="{uri}",'
                        f'response="{resp}",algorithm=AKAv1-MD5,'
                        f'auts="{base64.b64encode(aka["auts"]).decode()}"')
            else:
                raise RuntimeError("AKA resynchronisation did not converge")
            offers = [x.strip() for v in hall(r, "security-server") for x in v.split(",") if x.strip()]
            sv = pick_sec_offer(offers)
            ss = ", ".join(offers)                  # RFC 3329: echo every offer in Security-Verify
            pc_c, pc_s = int(sv["port-c"]), int(sv["port-s"])
            for x in new:
                self.peers[x] = {(pcscf, pc_c), (pcscf, pc_s)}
            handle = self.ipsec.setup(self.pdn.addr, pcscf, ue_c, ue_s, pc_c, pc_s,
                                      spi_uc, spi_us, int(sv["spi-c"]), int(sv["spi-s"]), aka["ik"])
            resp = digest_response(self.impi, realm, nonce, aka["res"], "REGISTER", uri)
            auth = (f'Digest username="{self.impi}",realm="{realm}",nonce="{nonce}",'
                    f'uri="{uri}",response="{resp}",algorithm=AKAv1-MD5')
            r = self.send_request(self._register_msg(ue_c, ue_s, sec_client, auth=auth, sec_verify=ss),
                                  (pcscf, pc_s), sock=sock_c)
            if not r or r["code"] != 200:
                raise RuntimeError(f"protected REGISTER: {r and r['start']}")
        except Exception:
            if handle:
                self.ipsec.remove(handle)
            self.extra_socks = []
            self._close(new)
            raise
        # success: switch to the new registration, then retire the old one
        old = self.socks
        self.pcscf, self.pc_s, self.ue_c, self.ue_s = pcscf, pc_s, ue_c, ue_s
        self.sock_c, self.sock_s = new[0], new[1]
        self.socks = new
        self.extra_socks = []
        self.pi ^= 1
        self._close(old)
        self.ipsec.flush_old(keep=1)
        self.last_reg = (pcscf, new[0], ue_c, ue_s, pc_s, sec_client, auth, ss)
        self.registered = True
        self.m.send_line("REG 1")
        self.assoc = [re.sub(r"[<>]", "", x.strip()) for v in hall(r, "p-associated-uri") for x in v.split(",")]
        self.service_route = [x.strip() for v in hall(r, "service-route") for x in v.split(",")]
        exp = None
        for v in hall(r, "contact"):
            m = re.search(r"expires=(\d+)", v)
            if m:
                exp = int(m.group(1))
        self.expires = exp or int(h(r, "expires") or 3600)
        log(f"REGISTERED via {pcscf} for {self.expires}s; identities {self._mask(self.assoc)}")

    def on_bearer_lost(self):
        self.registered = False
        try:
            self.m.send_line("REG 0")
        except OSError:
            pass
        self.wake.set()

    def deregister(self):
        """Best effort REGISTER Expires: 0 over the protected channel (on shutdown), so the
        network stops routing SMS to a contact whose SAs are about to disappear."""
        if not self.registered or not self.last_reg:
            return
        pcscf, sock, ue_c, ue_s, pc_s, sec_client, auth, ss = self.last_reg
        self.registered = False
        try:
            # TS 24.229 5.1.1.6: over the existing SAs; the P-CSCF vouches for integrity
            raw = self._register_msg(ue_c, ue_s, None, auth=auth, sec_verify=ss, expires=0)
            r = self.send_request(raw, (pcscf, pc_s), timeout=3, sock=sock)
            if r and r["code"] == 401:          # the S-CSCF re-challenges: answer once
                www = h(r, "www-authenticate")
                nonce = re.search(r'nonce="([^"]+)"', www).group(1)
                realm = re.search(r'realm="([^"]+)"', www).group(1)
                nr = base64.b64decode(nonce)
                aka = self.m.aka(nr[:16], nr[16:32])
                uri = f"sip:{self.domain}"
                resp = digest_response(self.impi, realm, nonce, aka["res"], "REGISTER", uri)
                auth = (f'Digest username="{self.impi}",realm="{realm}",nonce="{nonce}",'
                        f'uri="{uri}",response="{resp}",algorithm=AKAv1-MD5')
                raw = self._register_msg(ue_c, ue_s, None, auth=auth, sec_verify=ss, expires=0)
                r = self.send_request(raw, (pcscf, pc_s), timeout=3, sock=sock)
            why = " ".join(filter(None, (h(r, "warning"), h(r, "reason")))) if r else ""
            log(f"deregister: {r['start'] if r else 'no answer'} {why}".rstrip())
        except Exception as e:
            log("deregister failed:", repr(e))

    def _register_msg(self, pc, ps, sec_client, auth, sec_verify=None, expires=600000):
        self.cseq += 1
        a = self.pdn.addr
        lines = [
            f"REGISTER sip:{self.domain} SIP/2.0",
            f"Via: SIP/2.0/UDP [{a}]:{pc};branch={branch()};rport",
            "Max-Forwards: 70",
            f"From: <{self.impu}>;tag={tag()}",
            f"To: <{self.impu}>",
            f"Call-ID: {self.callid}",
            f"CSeq: {self.cseq} REGISTER",
            f'Contact: <sip:{self.imsi}@[{a}]:{ps}>;+sip.instance="<{self.instance}>";+g.3gpp.smsip;'
            f'+g.3gpp.icsi-ref="urn%3Aurn-7%3A3gpp-service.ims.icsi.mmtel"'
            + (";expires=0" if expires == 0 else ""),
            f"Expires: {expires}",
            "Supported: path,gruu,sec-agree",
            "Require: sec-agree",
            "Proxy-Require: sec-agree",
        ]
        if sec_client:
            lines.append(f"Security-Client: {sec_client}")
        if sec_verify:
            lines.append(f"Security-Verify: {sec_verify}")
        if auth:
            lines.append(f"Authorization: {auth}")
        else:
            lines.append(f'Authorization: Digest username="{self.impi}",realm="{self.domain}",'
                         f'uri="sip:{self.domain}",nonce="",response=""')
        lines += ["Allow: INVITE,ACK,CANCEL,BYE,UPDATE,PRACK,MESSAGE,OPTIONS,NOTIFY",
                  "User-Agent: Q25-Linux-IMS/0.1", "Content-Length: 0", "", ""]
        return "\r\n".join(lines)

    # -------- incoming requests
    def reply(self, req, send, code, reason, extra=()):
        lines = [f"SIP/2.0 {code} {reason}"]
        for k in ("via", "from", "to", "call-id", "cseq"):
            for v in hall(req, k):
                if k == "to" and "tag=" not in v:
                    v += f";tag={tag()}"
                lines.append(f"{k.title() if k != 'call-id' else 'Call-ID'}: {v}")
        lines += list(extra) + ["Content-Length: 0", "", ""]
        send("\r\n".join(lines).encode())

    def on_request(self, req, send):
        """Runs on the receive thread: answer at once, queue the real work."""
        meth = req["method"]
        key = (h(req, "call-id"), h(req, "cseq"))
        dup = key in self.seen
        now = time.time()
        self.seen = {k: t for k, t in self.seen.items() if now - t < 300}
        self.seen[key] = now
        log(f"<- {meth}{' (retransmission)' if dup else ''} {len(req['body'])} body bytes")
        if meth == "MESSAGE":
            self.reply(req, send, 200, "OK")
            if dup:
                return
            ctype = (h(req, "content-type") or "").lower()
            if "vnd.3gpp.sms" in ctype:
                body = req["body"]
                if body and body[0] & 0x07 in (smspdu.RP_ACK_N_MS, smspdu.RP_ERROR_N_MS):
                    self.on_sms_rp(req)          # never blocks: just wakes the MO waiter
                else:
                    self.work.put((self.on_sms_rp, (req,)))
            else:
                log("MESSAGE with content-type", ctype)
        elif meth in ("OPTIONS", "NOTIFY"):
            self.reply(req, send, 200, "OK")
        else:
            self.reply(req, send, 405, "Method Not Allowed")

    def on_sms_rp(self, req):
        rp = smspdu.decode_rp(req["body"])
        if rp["type"] == smspdu.RP_DATA_N_MS:
            handed = self.hand_to_mm(rp["oa"], rp["tpdu"])
            # acknowledge to the SMSC only once the message is safely with ModemManager or,
            # failing that, in the local store (a crash before this point makes the network retry)
            if not handed:
                self.deliver(smspdu.decode_deliver(rp["tpdu"]))
            self._send_rp(smspdu.rp_ack_ms_n(rp["mr"]), in_reply_to=h(req, "call-id"),
                          to=re.search(r"<([^>]+)>", h(req, "from")).group(1), wait=False)
        elif rp["type"] in (smspdu.RP_ACK_N_MS, smspdu.RP_ERROR_N_MS):
            ok = rp["type"] == smspdu.RP_ACK_N_MS
            log(f"{'RP-ACK' if ok else 'RP-ERROR'} for MR {rp['mr']}"
                + ("" if ok else f": cause {rp.get('cause')}"))
            w = self.mo_wait.get(rp["mr"])
            if w:
                w[1] = (ok, rp.get("cause"))
                w[0].set()
        else:
            log(f"unhandled RP message type {rp['type']}")

    def hand_to_mm(self, smsc, tpdu):
        while not self.m.mtres.empty():
            self.m.mtres.get_nowait()
        self.m.send_line(f"MT {smsc or '-'} {tpdu.hex()}")
        try:
            res = self.m.mtres.get(timeout=10)
        except queue.Empty:
            res = "ERR timeout"
        if res.startswith("OK"):
            log("SMS part handed to ModemManager")
            return True
        log("ModemManager did not take the SMS:", res)
        return False

    def deliver(self, d):
        if d["concat"]:
            ref, total, seq = d["concat"]
            key = (d["from"], ref)
            now = time.time()
            self.concat = {k: v for k, v in self.concat.items() if now - v["t"] < 86400}
            ent = self.concat.setdefault(key, {"t": now, "parts": {}})
            ent["parts"][seq] = d["text"]
            if len(ent["parts"]) < total:
                return
            d = dict(d, text="".join(ent["parts"][i] for i in sorted(ent["parts"])))
            del self.concat[key]
        rec = {"from": d["from"], "time": d["time"], "text": d["text"], "received": time.time()}
        path = os.path.join(STATE_DIR, "sms.jsonl")
        try:
            if os.path.getsize(path) > SMS_LOG_MAX:
                os.replace(path, path + ".1")
        except OSError:
            pass
        fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
        with os.fdopen(fd, "a") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
        log(f"SMS received from {mask_num(d['from'])} ({len(d['text'])} chars)")
        u = desktop_user()
        if not u:
            return
        try:
            subprocess.run(["runuser", "-u", u.pw_name, "--", "env",
                            f"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{u.pw_uid}/bus",
                            "notify-send", "-a", "SMS", f"SMS from {d['from']}", d["text"]],
                           check=False, timeout=10)
        except subprocess.TimeoutExpired:
            log("notification timed out")

    # -------- outgoing
    def _send_rp(self, body, to, in_reply_to=None, wait=True):
        with self.mr_lock:
            self.cseq += 1
        a = self.pdn.addr
        from_uri = next((u for u in self.assoc if u.startswith("sip:")), self.impu)
        lines = [f"MESSAGE {to} SIP/2.0",
                 f"Via: SIP/2.0/UDP [{a}]:{self.ue_c};branch={branch()};rport",
                 "Max-Forwards: 70"]
        lines += [f"Route: {r}" for r in self.service_route]
        lines += [f"From: <{from_uri}>;tag={tag()}", f"To: <{to}>",
                  f"Call-ID: {'%016x' % random.getrandbits(64)}@q25", f"CSeq: {self.cseq} MESSAGE",
                  f"P-Preferred-Identity: <{from_uri}>",
                  "Content-Type: application/vnd.3gpp.sms",
                  "Request-Disposition: no-fork",
                  "Accept-Contact: *;+g.3gpp.smsip",
                  "Allow: MESSAGE"]
        if in_reply_to:
            lines.append(f"In-Reply-To: {in_reply_to}")
        lines += [f"Content-Length: {len(body)}", "", ""]
        raw = "\r\n".join(lines).encode() + body
        b = re.search(rb"branch=([^;\r\n]+)", raw).group(1).decode()
        ev = threading.Event()
        self.pending[b] = [ev, None]
        self.sock_c.sendto(raw, (self.pcscf, self.pc_s))
        if not wait:
            self.pending.pop(b, None)
            return None
        end, r = time.time() + 15, None
        while time.time() < end:
            ev.wait(max(0.0, end - time.time()))
            ev.clear()
            r = (self.pending.get(b) or [None, None])[1]
            if r and r["code"] >= 200:
                break
        self.pending.pop(b, None)
        return r

    def send_tpdu(self, tpdu):
        """RP-DATA(SMS-SUBMIT) to the SMSC; True once the SMSC answers RP-ACK."""
        if not self.registered:
            return False, "not registered"
        with self.mr_lock:
            self.mr = (self.mr + 1) & 0xFF
            mr = self.mr
        w = [threading.Event(), None]
        self.mo_wait[mr] = w
        try:
            sc = f"sip:{self.smsc}@{self.domain};user=phone"
            r = self._send_rp(smspdu.rp_data_ms_n(mr, self.smsc, tpdu), to=sc)
            if not r or r["code"] >= 300:
                return False, r["start"] if r else "no SIP answer"
            if not w[0].wait(30):
                return False, "no RP-ACK from SMSC"
            ok, cause = w[1]
            return ok, "" if ok else f"RP cause {cause}"
        finally:
            self.mo_wait.pop(mr, None)

    def on_mo(self, rid, tpdu):
        def job():
            try:
                ok, why = self.send_tpdu(tpdu)
            except Exception as e:
                ok, why = False, repr(e)
            log(f"MO {rid}: {'accepted by SMSC' if ok else 'failed: ' + why}")
            self.m.send_line(f"MORES {rid} OK" if ok else f"MORES {rid} ERR {why}")
        threading.Thread(target=job, daemon=True).start()

    def send_sms(self, number, text):
        if not self.registered:
            raise RuntimeError("not registered")
        results = []
        for tpdu in smspdu.encode_submit(number, text):
            ok, why = self.send_tpdu(tpdu)
            results.append("RP-ACK" if ok else why)
        log(f"SMS to {mask_num(number)}: {results}")
        return results

    # -------- control socket
    def ctl_loop(self):
        if os.path.exists(CTL):
            os.unlink(CTL)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(CTL)
        os.chmod(CTL, 0o600)
        srv.listen(4)
        while True:
            c, _ = srv.accept()
            try:
                req = json.loads(c.recv(65536).decode())
                if req.get("cmd") == "send":
                    out = {"ok": True, "result": self.send_sms(req["to"], req["text"])}
                else:
                    out = {"ok": True, "registered": self.registered, "pdn": self.pdn.ifname,
                           "pcscf": self.pdn.pcscf, "expires": self.expires}
            except Exception as e:
                out = {"ok": False, "error": repr(e)}
            c.sendall(json.dumps(out).encode())
            c.close()

    def _mask(self, xs):
        return [mask_num(x.replace(self.imsi, self.imsi[:5] + "********" + self.imsi[-2:])) for x in xs]

    def run(self):
        def stop(signum, _frame):
            log(f"signal {signum}: deregistering")
            self.deregister()
            self.ipsec.flush_all()
            os._exit(0)
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        threading.Thread(target=self.ctl_loop, daemon=True).start()
        # the receiver must run before REGISTER: responses are matched there
        threading.Thread(target=self.rx_loop, daemon=True).start()
        threading.Thread(target=self.worker_loop, daemon=True).start()
        self.m.mo_handler = self.on_mo
        self.pdn.on_lost = self.on_bearer_lost
        self.m.send_line("REG 0")
        rereg = int(os.environ.get("Q25_IMSD_REREG", "0"))
        backoff = 10
        while True:
            try:
                self.register()
                backoff = 10
                deadline = time.time() + (rereg or max(60, self.expires // 2))
                while time.time() < deadline:
                    if self.wake.wait(min(60, max(1, deadline - time.time()))):
                        break
                    if self.pdn.addr and self.pdn.addr not in self.pdn.global_addrs():
                        log("IMS address changed; re-registering")
                        self.pdn.addr = None     # bearer still up: just re-read the address
                        break
                self.wake.clear()
                log("re-registering")
            except Exception as e:
                self.registered = False
                try:
                    self.m.send_line("REG 0")
                except OSError:
                    pass
                log("registration failed:", repr(e), f"; retry in {backoff}s")
                self.wake.wait(backoff)
                self.wake.clear()
                backoff = min(backoff * 2, 600)


def pick_sec_offer(offers):
    """Choose the best Security-Server mechanism we can actually install
    (ipsec-3gpp, hmac-sha-1-96 integrity, null encryption)."""
    good = []
    for o in offers:
        p = dict(re.findall(r"([a-z-]+)=([^;,\s]+)", o.lower()))
        if not o.lower().startswith("ipsec-3gpp"):
            continue
        if p.get("alg") != "hmac-sha-1-96" or p.get("ealg", "null") != "null":
            continue
        if not all(k in p for k in ("spi-c", "spi-s", "port-c", "port-s")):
            continue
        good.append((float(p.get("q", "0.1")), p))
    if not good:
        raise RuntimeError(f"no usable Security-Server offer in {offers}")
    return max(good, key=lambda x: x[0])[1]


def mask_num(s):
    """Keep logs free of full phone numbers."""
    return re.sub(r"(\+?\d{3})\d{4,}(\d{3})", r"\1****\2", s)


def digest_response(username, realm, nonce, res, method, uri):
    """RFC 3310 Digest AKAv1-MD5 (no qop): the password is RES as raw octets."""
    ha1 = hashlib.md5(f"{username}:{realm}:".encode() + res).hexdigest()
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    return hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()


def client(req):
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.connect(CTL)
    c.sendall(json.dumps(req).encode())
    print(c.recv(65536).decode())


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "send":
        client({"cmd": "send", "to": sys.argv[2], "text": " ".join(sys.argv[3:])})
    elif len(sys.argv) > 1 and sys.argv[1] == "status":
        client({"cmd": "status"})
    else:
        Ims().run()
