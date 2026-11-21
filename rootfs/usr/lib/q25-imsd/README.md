# q25-imsd — SMS over IMS (prototype, phase A)

Native IMS client for the Q25's MT6789 modem: no MediaTek IMS daemons, no Android
userspace. Verified 2026-11-22 on China Telecom (46011): registration, receiving
and sending SMS.

How it works (3GPP TS 24.229 / 24.341 / 33.203):

1. Modem: `AT+EIMSCFG=1,0,0,0,1,1`, `AT+EIMS=1`, add an `ims` APN (`+EAPNSET`,
   type=ims) and activate it (`+EAPNACT=1,"ims","ims",0`). The modem reports the
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

Phase A limits: drives the modem through ModemManager's debug command API
(`ModemManager --debug`); received SMS go to `/var/lib/q25-imsd/sms.jsonl` + a
desktop notification; sending via `q25-imsd.py send <number> <text>`.
Phase B: move bearer/AKA/SMS hand-off into the ModemManager plugin so Chatty
works natively and MM runs without --debug.

Tests: `python3 test_smspdu.py`.
