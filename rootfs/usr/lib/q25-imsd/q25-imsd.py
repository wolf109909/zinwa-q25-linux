#!/usr/bin/env python3
"""q25-imsd: minimal IMS client for SMS over IP on the Zinwa Q25 (MT6789 modem).

Phase A prototype. Talks to the modem through ModemManager's debug command API
(mmcli --command), brings up the IMS PDN, registers to the operator IMS core with
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
import random
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
USER = "max"


def log(*a):
    print(f"{time.strftime('%H:%M:%S')} {LOG_PREFIX}:", *a, flush=True)


def sh(*cmd, check=True):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f"{' '.join(cmd)}: {r.stderr.strip()}")
    return r.stdout


# ---------------------------------------------------------------- modem (AT)
class Modem:
    lock = threading.Lock()

    def at(self, cmd, timeout=30):
        with self.lock:
            r = subprocess.run(["mmcli", "-m", "any", f"--command={cmd}", f"--timeout={timeout}"],
                               capture_output=True, text=True)
        out = (r.stdout + r.stderr).strip()
        if r.returncode or "error" in out.lower() and "response:" not in out:
            raise RuntimeError(f"{cmd}: {out}")
        m = re.search(r"response: '(.*)'", out, re.S)
        return m.group(1) if m else ""

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
            self.m.at("AT+EAPNLOCK=1")
            self.m.at('AT+EAPNSET="ims",2,"type=ims;protocol=IPV4V6;roaming_protocol=IPV4V6;'
                      'authtype=0;carrier_enabled=1;max_conns=0;max_conns_time=0;wait_time=0;'
                      'bearer_bitmask=0;inactive_timer=0"')
            self.m.at("AT+EAPNLOCK=0")
            since = time.time() - 1
            self.m.at('AT+EAPNACT=1,"ims","ims",0', timeout=90)
            eif = None
            for _ in range(20):
                j = sh("journalctl", "-u", "ModemManager", "-o", "cat", "--no-pager",
                       "--since", f"@{int(since)}", check=False)
                m = re.findall(r"\+EIF: (\d+), ifup, 2", j)
                if m:
                    eif = int(m[-1])
                    break
                time.sleep(1)
            if eif is None:
                raise RuntimeError("no +EIF for the IMS PDN")
            self.m.at(f'AT+EIF={eif},"ifst","up",3')
            self.ifname = f"ccmni{eif % 100}"
            self.pcscf = self.contrdp()
        if not self.ifname:
            self.ifname = self._find_if()
        sh("ip", "link", "set", self.ifname, "up")
        for _ in range(30):
            a = re.findall(r"inet6 (2[0-9a-f:]+)/\d+ scope global", sh("ip", "-6", "addr", "show", "dev", self.ifname))
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

    def _find_if(self):
        for n in range(21):
            out = sh("ip", "-6", "addr", "show", "dev", f"ccmni{n}", check=False)
            if "proto kernel_ra" in out and "240e" in out:
                return f"ccmni{n}"
        # fall back to the most recent +EIF from the journal
        j = sh("journalctl", "-u", "ModemManager", "-o", "cat", "--no-pager", check=False)
        m = re.findall(r"\+EIF: (\d+), ifup, 2", j)
        if m:
            return f"ccmni{int(m[-1]) % 100}"
        raise RuntimeError("cannot find IMS ccmni interface")


# ---------------------------------------------------------------- IPsec
class IPsec:
    def __init__(self):
        self.active = []

    def setup(self, ue, pc, ue_c, ue_s, pc_c, pc_s, spi_uc, spi_us, spi_pc, spi_ps, ik):
        key = "0x" + (ik + b"\x00" * 4).hex()
        sas = [  # (src, dst, spi)
            (ue, pc, spi_ps),   # UE port-c -> P-CSCF port-s
            (ue, pc, spi_pc),   # UE port-s -> P-CSCF port-c
            (pc, ue, spi_us),   # P-CSCF port-c -> UE port-s
            (pc, ue, spi_uc),   # P-CSCF port-s -> UE port-c
        ]
        for src, dst, spi in sas:
            sh("ip", "xfrm", "state", "add", "src", src, "dst", dst, "proto", "esp", "spi", str(spi),
               "mode", "transport", "auth-trunc", "hmac(sha1)", key, "96", "enc", "cipher_null", "")
        pols = [  # (src, sport, dst, dport, dir, spi)
            (ue, ue_c, pc, pc_s, "out", spi_ps),
            (ue, ue_s, pc, pc_c, "out", spi_pc),
            (pc, pc_c, ue, ue_s, "in", spi_us),
            (pc, pc_s, ue, ue_c, "in", spi_uc),
        ]
        for src, sp, dst, dp, d, spi in pols:
            for proto in ("udp", "tcp"):
                sh("ip", "xfrm", "policy", "add", "src", src, "dst", dst, "proto", proto,
                   "sport", str(sp), "dport", str(dp), "dir", d,
                   "tmpl", "src", src, "dst", dst, "proto", "esp", "spi", str(spi), "mode", "transport")
        self.active.append((sas, pols))

    def flush_old(self, keep=1):
        while len(self.active) > keep:
            sas, pols = self.active.pop(0)
            for src, sp, dst, dp, d, _ in pols:
                for proto in ("udp", "tcp"):
                    sh("ip", "xfrm", "policy", "del", "src", src, "dst", dst, "proto", proto,
                       "sport", str(sp), "dport", str(dp), "dir", d, check=False)
            for src, dst, spi in sas:
                sh("ip", "xfrm", "state", "del", "src", src, "dst", dst, "proto", "esp", "spi", str(spi),
                   check=False)

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
        self.concat = {}
        self.work = queue.Queue()
        self.seen = {}               # (call-id, cseq) -> time, to drop retransmissions
        os.makedirs(STATE_DIR, exist_ok=True)

    # -------- transport
    def _open_sockets(self, pc, ps):
        for s in (self.sock_c, self.sock_s):
            if s:
                s.close()
        self.sock_c = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        self.sock_c.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock_c.bind((self.pdn.addr, pc))
        self.sock_s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        self.sock_s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock_s.bind((self.pdn.addr, ps))

    def send_request(self, raw, dest_port, timeout=10):
        b = re.search(r"branch=([^;\r\n]+)", raw).group(1)
        ev = threading.Event()
        self.pending[b] = [ev, None]
        self.sock_c.sendto(raw.encode() if isinstance(raw, str) else raw, (self.pcscf, dest_port))
        final = None
        end = time.time() + timeout
        while time.time() < end:
            ev.wait(max(0.0, end - time.time()))
            ev.clear()
            r = self.pending[b][1]
            if r and r["code"] >= 200:
                final = r
                break
        self.pending.pop(b, None)
        return final

    def rx_loop(self):
        while True:
            socks = [s for s in (self.sock_c, self.sock_s) if s]
            try:
                rd, _, _ = select.select(socks, [], [], 1.0)
            except (OSError, ValueError):
                time.sleep(0.2)
                continue
            for s in rd:
                try:
                    data, addr = s.recvfrom(65535)
                except OSError:
                    continue
                if not data.strip():
                    continue
                try:
                    msg = parse_sip(data)
                except Exception as e:
                    log("unparsable SIP:", e)
                    continue
                if "code" in msg:
                    b = re.search(r"branch=([^;\s,]+)", h(msg, "via") or "")
                    if b and b.group(1) in self.pending:
                        self.pending[b.group(1)][1] = msg
                        self.pending[b.group(1)][0].set()
                else:
                    try:
                        self.on_request(msg, s, addr)
                    except Exception as e:
                        log("request handling error:", repr(e))

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
        self.pcscf = self.pdn.pcscf[0]
        ue_c, ue_s = self.ports[self.pi]
        self.pi ^= 1
        self._open_sockets(ue_c, ue_s)
        spi_uc, spi_us = random.randint(0x10000, 0x7fffffff), random.randint(0x10000, 0x7fffffff)
        sec_client = (f"ipsec-3gpp;alg=hmac-sha-1-96;ealg=null;spi-c={spi_uc};spi-s={spi_us};"
                      f"port-c={ue_c};port-s={ue_s}")
        r = self.send_request(self._register_msg(ue_c, ue_s, sec_client, auth=None), 5060)
        if not r or r["code"] != 401:
            raise RuntimeError(f"initial REGISTER: {r and r['start']}")
        www = h(r, "www-authenticate")
        nonce = re.search(r'nonce="([^"]+)"', www).group(1)
        realm = re.search(r'realm="([^"]+)"', www).group(1)
        ss = h(r, "security-server")
        sv = dict(re.findall(r"([a-z-]+)=([^;,\s]+)", ss))
        raw = base64.b64decode(nonce)
        aka = self.m.aka(raw[:16], raw[16:32])
        if "auts" in aka:
            raise RuntimeError("AKA synchronisation failure (AUTS); resync not implemented yet")
        pc_c, pc_s = int(sv["port-c"]), int(sv["port-s"])
        self.ipsec.setup(self.pdn.addr, self.pcscf, ue_c, ue_s, pc_c, pc_s,
                         spi_uc, spi_us, int(sv["spi-c"]), int(sv["spi-s"]), aka["ik"])
        self.pc_s = pc_s
        resp = digest_response(self.impi, realm, nonce, aka["res"], "REGISTER", f"sip:{self.domain}")
        auth = (f'Digest username="{self.impi}",realm="{realm}",nonce="{nonce}",'
                f'uri="sip:{self.domain}",response="{resp}",algorithm=AKAv1-MD5')
        r = self.send_request(self._register_msg(ue_c, ue_s, sec_client, auth=auth, sec_verify=ss), pc_s)
        if not r or r["code"] != 200:
            raise RuntimeError(f"protected REGISTER: {r and r['start']}")
        self.ipsec.flush_old(keep=1)
        self.registered = True
        self.assoc = [re.sub(r"[<>]", "", x.strip()) for v in hall(r, "p-associated-uri") for x in v.split(",")]
        self.service_route = [x.strip() for v in hall(r, "service-route") for x in v.split(",")]
        exp = None
        for v in hall(r, "contact"):
            m = re.search(r"expires=(\d+)", v)
            if m:
                exp = int(m.group(1))
        self.expires = exp or int(h(r, "expires") or 3600)
        self.ue_c, self.ue_s = ue_c, ue_s
        log(f"REGISTERED for {self.expires}s; identities {self._mask(self.assoc)}")

    def _register_msg(self, pc, ps, sec_client, auth, sec_verify=None):
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
            f'+g.3gpp.icsi-ref="urn%3Aurn-7%3A3gpp-service.ims.icsi.mmtel"',
            "Expires: 600000",
            "Supported: path,gruu,sec-agree",
            "Require: sec-agree",
            "Proxy-Require: sec-agree",
            f"Security-Client: {sec_client}",
        ]
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
    def reply(self, req, sock, addr, code, reason, extra=()):
        lines = [f"SIP/2.0 {code} {reason}"]
        for k in ("via", "from", "to", "call-id", "cseq"):
            for v in hall(req, k):
                if k == "to" and "tag=" not in v:
                    v += f";tag={tag()}"
                lines.append(f"{k.title() if k != 'call-id' else 'Call-ID'}: {v}")
        lines += list(extra) + ["Content-Length: 0", "", ""]
        sock.sendto("\r\n".join(lines).encode(), addr)

    def on_request(self, req, sock, addr):
        """Runs on the receive thread: answer at once, queue the real work."""
        meth = req["method"]
        key = (h(req, "call-id"), h(req, "cseq"))
        dup = key in self.seen
        now = time.time()
        self.seen = {k: t for k, t in self.seen.items() if now - t < 300}
        self.seen[key] = now
        log(f"<- {meth}{' (retransmission)' if dup else ''} {len(req['body'])} body bytes")
        if meth == "MESSAGE":
            self.reply(req, sock, addr, 200, "OK")
            if dup:
                return
            ctype = (h(req, "content-type") or "").lower()
            if "vnd.3gpp.sms" in ctype:
                self.work.put((self.on_sms_rp, (req,)))
            else:
                log("MESSAGE with content-type", ctype)
        elif meth in ("OPTIONS", "NOTIFY"):
            self.reply(req, sock, addr, 200, "OK")
        else:
            self.reply(req, sock, addr, 405, "Method Not Allowed")

    def on_sms_rp(self, req):
        rp = smspdu.decode_rp(req["body"])
        if rp["type"] == smspdu.RP_DATA_N_MS:
            d = smspdu.decode_deliver(rp["tpdu"])
            self._send_rp(smspdu.rp_ack_ms_n(rp["mr"]), in_reply_to=h(req, "call-id"),
                          to=re.search(r"<([^>]+)>", h(req, "from")).group(1))
            self.deliver(d)
        elif rp["type"] == smspdu.RP_ACK_N_MS:
            log(f"RP-ACK for MR {rp['mr']}: SMS accepted by the SMSC")
        elif rp["type"] == smspdu.RP_ERROR_N_MS:
            log(f"RP-ERROR for MR {rp['mr']}: cause {rp.get('cause')}")

    def deliver(self, d):
        if d["concat"]:
            ref, total, seq = d["concat"]
            key = (d["from"], ref)
            parts = self.concat.setdefault(key, {})
            parts[seq] = d["text"]
            if len(parts) < total:
                return
            d = dict(d, text="".join(parts[i] for i in sorted(parts)))
            del self.concat[key]
        rec = {"from": d["from"], "time": d["time"], "text": d["text"], "received": time.time()}
        with open(os.path.join(STATE_DIR, "sms.jsonl"), "a") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
        log(f"SMS received from {mask_num(d['from'])} ({len(d['text'])} chars)")
        uid = int(sh("id", "-u", USER).strip())
        try:
            subprocess.run(["runuser", "-u", USER, "--", "env",
                            f"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{uid}/bus",
                            "notify-send", "-a", "SMS", f"SMS from {d['from']}", d["text"]],
                           check=False, timeout=10)
        except subprocess.TimeoutExpired:
            log("notification timed out")

    # -------- outgoing
    def _send_rp(self, body, to, in_reply_to=None):
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
        end, r = time.time() + 15, None
        while time.time() < end:
            ev.wait(max(0.0, end - time.time()))
            ev.clear()
            r = self.pending[b][1]
            if r and r["code"] >= 200:
                break
        self.pending.pop(b, None)
        return r

    def send_sms(self, number, text):
        if not self.registered:
            raise RuntimeError("not registered")
        sc = f"sip:{self.smsc}@{self.domain};user=phone"
        results = []
        for tpdu in smspdu.encode_submit(number, text):
            self.mr = (self.mr + 1) & 0xFF
            r = self._send_rp(smspdu.rp_data_ms_n(self.mr, self.smsc, tpdu), to=sc)
            results.append(r["start"] if r else "timeout")
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
        threading.Thread(target=self.ctl_loop, daemon=True).start()
        # the receiver must run before REGISTER: responses are matched there
        threading.Thread(target=self.rx_loop, daemon=True).start()
        threading.Thread(target=self.worker_loop, daemon=True).start()
        backoff = 10
        while True:
            try:
                self.register()
                backoff = 10
                time.sleep(max(60, self.expires // 2))
                log("re-registering")
            except Exception as e:
                self.registered = False
                log("registration failed:", repr(e), f"; retry in {backoff}s")
                time.sleep(backoff)
                backoff = min(backoff * 2, 600)


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
