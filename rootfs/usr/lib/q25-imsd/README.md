# q25-imsd — SMS over IMS

Native IMS client for the Q25's MT6789 modem: no MediaTek IMS daemons, no Android
userspace. Tested on China Telecom (46011): registration, receiving and sending
SMS.

How it works (3GPP TS 24.229 / 24.341 / 33.203):

1. Modem: `AT+EIMSCFG=1,0,0,0,1,1`, `AT+EIMS=1`, then activate the `ims` APN
   (written by the MM plugin with `+EAPNSET`, type=ims) (`+EAPNACT=1,"ims","ims",0`). The modem reports the
   interface with `+EIF: <id>, ifup, 2` (ccmni<id % 100>, IPv6); `+CGCONTRDP`
   gives the P-CSCF addresses.
2. Source-based routing: traffic from the IMS address leaves via the IMS ccmni
   (otherwise Wi-Fi IPv6 wins).
3. REGISTER -> 401 with AKAv1-MD5 challenge and `Security-Server` (operator
   requires IPsec: hmac-sha-1-96, ealg null, transport). The USIM computes
   RES/CK/IK (`AT+CSIM`, AUTHENTICATE 3G context). Four ESP SAs + policies via
   `ip xfrm` (kernel needs XFRM_USER, INET6_ESP, CRYPTO_NULL). Protected REGISTER
   -> 200 OK.
4. SMS: SIP MESSAGE with `application/vnd.3gpp.sms` (RP-DATA / SMS-DELIVER), reply
   200 + RP-ACK; sending = RP-DATA(SMS-SUBMIT) to the SMSC from `+CSCA`.

Identities are derived from the IMSI (the SIM has no ISIM).

Integration (phase B):

- ModemManager plugin `mtk-ccci` (`packages/modemmanager-q25`, pkgrel 6) owns the
  only AT port (ttyC0; the other CCCI ttys don't speak AT). Its IMS bridge
  (`mm-ims-bridge-mtk-ccci.c`) serves the root-only socket `/run/q25-ims-bridge.sock`:
  `AT` passthrough on MM's serialized port, `+EIF`/IMS URC forwarding, `REG 0|1`,
  `MT <smsc> <tpdu>` (received SMS -> `mm_iface_modem_messaging_take_part`, so
  Chatty sees normal SMS incl. multipart and delivery reports) and `MO <id> <tpdu>`
  (`MMSmsMtkIms.send` routes SMS over IMS while registered, else AT +CMGS).
  The plugin also keeps an `ims` entry in the modem APN table. No `--debug`.
- `q25-imsd.service` (BindsTo ModemManager, Restart=always). The IMS bearer's
  +EIF id is kept in `/run/q25-imsd.pdn`. The SAs/policies it installs are
  recorded in `/run/q25-imsd.xfrm` and removed by the next instance (other xfrm
  users are left alone); keys go to `ip -batch` on stdin, never on a command line.
- The SMS centre is answered (RP-ACK) once MM took the message or, failing that, it
  is in `/var/lib/q25-imsd/sms.jsonl` (0600, rotated at 1 MiB). MO completes on the
  SMSC's RP-ACK.

Robustness:

- Re-registration at half the granted expiry, as a transaction: new ports, SAs and
  sockets are built next to the old ones and only switched over on 200 OK.
- P-CSCF failover over every address from `+CGCONTRDP`; AKA resynchronisation
  (AUTS) when the USIM reports a sequence error.
- `+EIF ifdown` of the IMS bearer or a changed IMS address triggers an immediate
  re-registration.
- SIP over UDP and TCP (listener on port-s, Content-Length framing, max 8 conns).
- Only the P-CSCF may talk to us: packets from any other address/port are dropped,
  and requests are accepted only on the ESP-protected ports.
- SIGTERM sends REGISTER Expires: 0 (answering the S-CSCF re-challenge) so the
  network stops routing SMS to a dead contact.
- `Q25_IMSD_REREG=<s>` forces a short re-registration interval for testing.

Tests: `python3 tests/test_q25_imsd_smspdu.py`.
