# Telephony

Mobile data and SMS work through stock ModemManager and NetworkManager, so Phosh,
GNOME Settings, Chatty, `nmcli` and `mmcli` work unchanged. Voice calls are not
implemented yet.

## Modem

The MT6789 modem runs its stock firmware from the stock `md1img` partition. The
kernel side is the ported MediaTek ECCCI stack (`drivers/mediatek/eccci`,
`ccmni`): CCIF control channels, the DPMAIF data path and `ccmniN` network
interfaces.

Booting the modem still needs the vendor's Android userspace helpers
(`ccci_rpcd`, `ccci_mdinit`), which provide the firmware filesystem and RPC
services. `q25-android.service` mounts the stock system and vendor images from
`super` read-only, and the `q25-ccci@` units run the helpers from there with
`libprop_preload` (`src/`) standing in for Android properties.

Only `ttyC0` speaks AT commands.

## Mobile data

`packages/modemmanager-q25` adds an `mtk-ccci` plugin to ModemManager 1.24.2:
AT on `ttyC0`, bearers through MediaTek's `+EAPNACT` commands, and the `ccmniN`
interface reported by `+EIF`. NetworkManager configures the interface as usual.

The modem needs the attach (initial EPS bearer) APN before it registers. Set it
in the NetworkManager mobile profile, or build the image with `Q25_APN=<apn>`.

## SMS over IMS

Operators without a circuit-switched network (tested on China Telecom) deliver
SMS only over IMS. `q25-imsd` (`rootfs/usr/lib/q25-imsd/`) is a small native IMS
client; no MediaTek IMS daemons are involved.

```
Chatty ── D-Bus ── ModemManager ── mtk-ccci plugin ── ttyC0 ── modem
                        │ IMS bridge (/run/q25-ims-bridge.sock)
                     q25-imsd ── IPsec (ip xfrm) ── ccmniN (IMS PDN) ── P-CSCF
```

1. `q25-imsd` enables the modem's IMS mode and brings up the `ims` PDN.
2. It registers with the P-CSCF using USIM AKA (`AT+CSIM`) and the operator's
   IPsec (sec-agree, ESP with HMAC-SHA1-96).
3. Received SMS (SIP MESSAGE with RP-DATA) are passed to ModemManager through the
   bridge and show up as normal messages, including multipart messages and
   delivery reports.
4. Sent SMS are routed by the plugin to `q25-imsd` while IMS is registered,
   otherwise through `AT+CMGS`.

The daemon re-registers before expiry, fails over to the secondary P-CSCF,
reacts to bearer loss, and deregisters on shutdown. Only traffic from the P-CSCF
is accepted. Details: [q25-imsd README](../rootfs/usr/lib/q25-imsd/README.md),
[ModemManager package README](../packages/modemmanager-q25/README.md).

## Not implemented

- Voice calls (VoLTE): needs SIP INVITE handling and an RTP/AMR audio path
  between the modem and the codec.
- USSD.
