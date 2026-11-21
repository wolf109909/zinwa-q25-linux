import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "rootfs", "usr", "lib", "q25-imsd"))
import smspdu as P
# 1) round trip SMS-SUBMIT -> decode as if DELIVER (same UD layout), GSM7 single
t = P.encode_submit("+8613800138000", "Your code is 123456")[0]
# convert submit -> deliver layout for decode test: first octet MTI 00, no MR, add PID/DCS/SCTS
def as_deliver(sub):
    first = sub[0] & 0x40
    i = 2; n = sub[i]; addr = sub[i:i + 2 + (n + 1)//2]; i += 2 + (n + 1)//2
    pid, dcs, udl = sub[i], sub[i+1], sub[i+2]; ud = sub[i+3:]
    return bytes([first]) + addr + bytes([pid, dcs]) + bytes.fromhex("62112202103523") + bytes([udl]) + ud
d = P.decode_deliver(as_deliver(t)); assert d["text"] == "Your code is 123456" and d["from"] == "+8613800138000", d
# 2) UCS-2 Chinese
t = P.encode_submit("10086", "验证码：654321，5分钟内有效")[0]
d = P.decode_deliver(as_deliver(t)); assert d["text"] == "验证码：654321，5分钟内有效", d
# 3) long GSM7 concatenated (200 chars) incl. escape chars
msg = ("ab{c}~" * 40)[:200]
parts = P.encode_submit("+15551234567", msg)
assert len(parts) == 2
txt = "".join(P.decode_deliver(as_deliver(p))["text"] for p in parts); assert txt == msg, (txt, msg)
assert all(P.decode_deliver(as_deliver(p))["concat"][1] == 2 for p in parts)
# 4) long UCS2
msg = "测试" * 50
parts = P.encode_submit("10086", msg); assert len(parts) == 2
assert "".join(P.decode_deliver(as_deliver(p))["text"] for p in parts) == msg
# 5) known real-world DELIVER PDU (GSM7 "hellohello"), from 3GPP examples
pdu = bytes.fromhex("040B911346610089F60000208062917314080CC8F71D14969741F977FD07")
d = P.decode_deliver(pdu); assert d["text"] == "How are you?" and d["from"] == "+31641600986", d
# 6) RP round trip
rp = P.rp_data_ms_n(5, "+8613344181200", t)
r = P.decode_rp(rp); assert r["type"] == 0 and r["mr"] == 5 and r["da"] == "+8613344181200" and r["tpdu"] == t, r
mt = bytes([1, 9]) + P.encode_rp_address("+8613344181200") + b"\x00" + bytes([len(pdu)]) + pdu
r = P.decode_rp(mt); assert r["type"] == 1 and r["oa"] == "+8613344181200" and r["tpdu"] == pdu
print("smspdu: all tests passed")
