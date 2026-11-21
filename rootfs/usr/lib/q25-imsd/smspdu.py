"""SMS TPDU / RP-PDU helpers (3GPP TS 23.040, 24.011, 23.038). Stdlib only."""
import datetime
import random

GSM7 = ("@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ\x1bÆæßÉ !\"#¤%&'()*+,-./0123456789:;<=>?"
        "¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà")
GSM7_EXT = {0x0A: "\f", 0x14: "^", 0x28: "{", 0x29: "}", 0x2F: "\\", 0x3C: "[",
            0x3D: "~", 0x3E: "]", 0x40: "|", 0x65: "€"}
GSM7_REV = {c: i for i, c in enumerate(GSM7)}
GSM7_EXT_REV = {c: i for i, c in GSM7_EXT.items()}


# ---- addresses -------------------------------------------------------------
def bcd_digits(data):
    out = ""
    for b in data:
        for nib in (b & 0x0F, b >> 4):
            if nib == 0x0F:
                return out
            out += "0123456789*#abc"[nib]
    return out


def to_bcd(digits):
    digits = digits.lstrip("+")
    if len(digits) % 2:
        digits += "F"
    return bytes(int(digits[i + 1], 16) << 4 | int(digits[i], 16) for i in range(0, len(digits), 2))


def decode_tp_address(data, i):
    """TP-OA/DA: length counts digits."""
    ndig, toa = data[i], data[i + 1]
    nbytes = (ndig + 1) // 2
    raw = data[i + 2:i + 2 + nbytes]
    if (toa >> 4) & 0x7 == 0x5:  # alphanumeric
        addr = unpack7(raw, ndig * 4 // 7)
    else:
        addr = bcd_digits(raw)[:ndig]
        if (toa >> 4) & 0x7 == 0x1:
            addr = "+" + addr
    return addr, i + 2 + nbytes


def encode_tp_address(number):
    intl = number.startswith("+")
    digits = number.lstrip("+")
    return bytes([len(digits), 0x91 if intl else 0x81]) + to_bcd(digits)


def encode_rp_address(number):
    """RP-OA/DA: length counts octets (incl. TON/NPI)."""
    if not number:
        return b"\x00"
    body = bytes([0x91 if number.startswith("+") else 0x81]) + to_bcd(number)
    return bytes([len(body)]) + body


def decode_rp_address(data, i):
    n = data[i]
    if n == 0:
        return "", i + 1
    toa = data[i + 1]
    addr = bcd_digits(data[i + 2:i + 1 + n])
    if (toa >> 4) & 0x7 == 0x1:
        addr = "+" + addr
    return addr, i + 1 + n


# ---- GSM 7-bit ---------------------------------------------------------------
def unpack7(data, nchars, skip_bits=0):
    bits = int.from_bytes(data, "little")
    bits >>= skip_bits
    out, esc = [], False
    for k in range(nchars):
        c = (bits >> (7 * k)) & 0x7F
        if esc:
            out.append(GSM7_EXT.get(c, "?"))
            esc = False
        elif c == 0x1B:
            esc = True
        else:
            out.append(GSM7[c])
    return "".join(out)


def _unpack7_skip(data, nsept, skip):
    bits = int.from_bytes(data, "little")
    out, esc = [], False
    for k in range(skip, nsept):
        c = (bits >> (7 * k)) & 0x7F
        if esc:
            out.append(GSM7_EXT.get(c, "?"))
            esc = False
        elif c == 0x1B:
            esc = True
        else:
            out.append(GSM7[c])
    return "".join(out)


def gsm7_septets(text):
    """Return septet list or None if text is not GSM-7 encodable."""
    out = []
    for ch in text:
        if ch in GSM7_REV:
            out.append(GSM7_REV[ch])
        elif ch in GSM7_EXT_REV:
            out += [0x1B, GSM7_EXT_REV[ch]]
        else:
            return None
    return out


def pack7(septets, fill_bits=0):
    bits, n = 0, fill_bits
    for s in septets:
        bits |= s << n
        n += 7
    return bits.to_bytes((n + 7) // 8, "little")


# ---- SMS-DELIVER (MT) ------------------------------------------------------
def decode_deliver(tpdu):
    first = tpdu[0]
    mti = first & 0x03
    if mti != 0x00:
        raise ValueError(f"not SMS-DELIVER (MTI {mti})")
    udhi = bool(first & 0x40)
    sender, i = decode_tp_address(tpdu, 1)
    pid, dcs = tpdu[i], tpdu[i + 1]
    scts = tpdu[i + 2:i + 9]
    i += 9
    udl = tpdu[i]
    ud = tpdu[i + 1:]
    concat = None
    hdr_len = 0
    if udhi:
        udhl = ud[0]
        hdr = ud[1:1 + udhl]
        hdr_len = udhl + 1
        j = 0
        while j < len(hdr):
            iei, l = hdr[j], hdr[j + 1]
            v = hdr[j + 2:j + 2 + l]
            if iei == 0x00 and l == 3:
                concat = (v[0], v[1], v[2])
            elif iei == 0x08 and l == 4:
                concat = ((v[0] << 8) | v[1], v[2], v[3])
            j += 2 + l
    alphabet = (dcs >> 2) & 0x03 if (dcs & 0xC0) == 0x00 else (2 if (dcs & 0xF0) == 0xE0 else 0)
    if (dcs & 0xF0) == 0xF0:
        alphabet = 1 if dcs & 0x04 else 0
    if alphabet == 0:   # GSM 7-bit: UDL counts septets incl. header + fill
        skip = (hdr_len * 8 + 6) // 7
        text = _unpack7_skip(ud, udl, skip)
    elif alphabet == 2:  # UCS-2
        text = ud[hdr_len:udl].decode("utf-16-be", errors="replace")
    else:
        text = ud[hdr_len:udl].hex()
    return {"from": sender, "pid": pid, "dcs": dcs, "time": decode_scts(scts),
            "text": text, "concat": concat}


def decode_scts(b):
    def sw(x):
        return (x & 0x0F) * 10 + (x >> 4)
    try:
        tz = (b[6] & 0x07) * 10 + (b[6] >> 4)
        tzq = -tz if b[6] & 0x08 else tz
        d = datetime.datetime(2000 + sw(b[0]), sw(b[1]), sw(b[2]), sw(b[3]), sw(b[4]), sw(b[5]),
                              tzinfo=datetime.timezone(datetime.timedelta(minutes=15 * tzq)))
        return d.isoformat()
    except Exception:
        return b.hex()


# ---- SMS-SUBMIT (MO) -------------------------------------------------------
def encode_submit(dest, text, mr=0):
    """Return list of TPDUs (concatenated if needed)."""
    septets = gsm7_septets(text)
    ref = random.randint(0, 255)
    parts = []
    if septets is not None:
        if len(septets) <= 160:
            chunks = [septets]
        else:
            chunks, cur = [], []
            for s in septets:          # 153 septets per part; don't split escapes
                if len(cur) >= 153 or (len(cur) == 152 and s == 0x1B):
                    chunks.append(cur)
                    cur = []
                cur.append(s)
            chunks.append(cur)
        for n, ch in enumerate(chunks, 1):
            if len(chunks) == 1:
                ud, udl, first = pack7(ch), len(ch), 0x01
            else:
                # 6-octet UDH = 48 bits -> 1 fill bit to the next septet boundary
                udh = bytes([5, 0x00, 3, ref, len(chunks), n])
                ud = udh + _pack7_after_header(ch)
                udl, first = 7 + len(ch), 0x41
            parts.append(bytes([first, mr]) + encode_tp_address(dest) + bytes([0x00, 0x00, udl]) + ud)
    else:
        data = text.encode("utf-16-be")
        chunks = [data] if len(data) <= 140 else [data[k:k + 134] for k in range(0, len(data), 134)]
        for n, ch in enumerate(chunks, 1):
            if len(chunks) == 1:
                ud, first = ch, 0x01
            else:
                ud, first = bytes([5, 0x00, 3, ref, len(chunks), n]) + ch, 0x41
            parts.append(bytes([first, mr]) + encode_tp_address(dest) + bytes([0x00, 0x08, len(ud)]) + ud)
    return parts


def _pack7_after_header(septets):
    # UDH (6 octets) + 1 fill bit: pack septets starting at bit 1 of the 7th octet
    bits, n = 0, 1
    for s in septets:
        bits |= s << n
        n += 7
    return bits.to_bytes((n + 7) // 8, "little")


# ---- RP layer (TS 24.011 7.3) -------------------------------------------------
RP_DATA_MS_N, RP_DATA_N_MS, RP_ACK_MS_N, RP_ACK_N_MS, RP_ERROR_MS_N, RP_ERROR_N_MS = 0, 1, 2, 3, 4, 5


def decode_rp(data):
    mtype, mr = data[0] & 0x07, data[1]
    out = {"type": mtype, "mr": mr}
    if mtype in (RP_DATA_N_MS, RP_DATA_MS_N):
        oa, i = decode_rp_address(data, 2)
        da, i = decode_rp_address(data, i)
        udl = data[i]
        out.update(oa=oa, da=da, tpdu=data[i + 1:i + 1 + udl])
    elif mtype in (RP_ERROR_N_MS, RP_ERROR_MS_N):
        out["cause"] = data[3] & 0x7F if len(data) > 3 else None
    return out


def rp_ack_ms_n(mr):
    # RP-ACK with an empty SMS-DELIVER-REPORT (TP-MTI=00, TP-PI=00)
    return bytes([RP_ACK_MS_N, mr, 0x41, 0x02, 0x00, 0x00])


def rp_data_ms_n(mr, smsc, tpdu):
    return bytes([RP_DATA_MS_N, mr]) + b"\x00" + encode_rp_address(smsc) + bytes([len(tpdu)]) + tpdu
