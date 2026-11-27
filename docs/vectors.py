"""Reference encoder for N2K-UPDATE.md (protocol version 1, revision 2).

Prints the test vectors quoted in Appendix A and a timing model of the
windowed transfer on the real images (Appendix B). Self-contained on purpose
(run with -I).
Usage: python -I vectors.py [<image> ...]
"""
import sys, zlib, hashlib, struct

BITRATE = 250_000
PGN = 126720
MFG = bytes([0xFE, 0x9F])          # (4 << 13) | (3 << 11) | 2046, little endian
PROTO = 1
T_QUERY, T_INFO, T_OFFER, T_REPLY, T_DATA, T_ACK, T_RESULT, T_APPLY, T_ABORT, T_STATUS, T_IDENT = range(0xA0, 0xAB)
CHUNK = 216                        # fixed in v1
WIN = 64
WEB_CAP = 0x20000                  # 128 KB work region = web UI cap (INFO byte 25)

def can_id(prio, pgn, src, dst):
    pf = (pgn >> 8) & 0xFF
    dp = (pgn >> 16) & 1
    assert pf < 240
    return (prio << 26) | (dp << 24) | (pf << 16) | (dst << 8) | src

def crc15(bits):
    crc = 0
    for b in bits:
        nxt = b ^ ((crc >> 14) & 1)
        crc = (crc << 1) & 0x7FFF
        if nxt:
            crc ^= 0x4599
    return crc

def frame_bits(cid, data):
    bits = [0]
    bits += [(cid >> (28 - i)) & 1 for i in range(11)]
    bits += [1, 1]
    bits += [(cid >> (17 - i)) & 1 for i in range(18)]
    bits += [0, 0, 0]
    dlc = len(data)
    bits += [(dlc >> (3 - i)) & 1 for i in range(4)]
    for byte in data:
        bits += [(byte >> (7 - i)) & 1 for i in range(8)]
    c = crc15(bits)
    bits += [(c >> (14 - i)) & 1 for i in range(15)]
    stuffed = 0
    run_val, run_len = None, 0
    for b in bits:
        if b == run_val:
            run_len += 1
        else:
            run_val, run_len = b, 1
        if run_len == 5:
            stuffed += 1
            run_val, run_len = 1 - b, 1
    return len(bits) + stuffed + 13

def fp_frames(payload, seq):
    assert 1 <= len(payload) <= 223
    out = [(bytes([(seq << 5) | 0, len(payload)]) + payload[:6]).ljust(8, b'\xff')]
    rest, n = payload[6:], 1
    while rest:
        out.append((bytes([(seq << 5) | n]) + rest[:7]).ljust(8, b'\xff'))
        rest, n = rest[7:], n + 1
    return out

def s(text, n):
    b = text.encode('utf-8')[:n]
    return b + b'\x00' * (n - len(b))

def hdr(t, sess):
    return MFG + bytes([t, PROTO, sess])

def query(what, sess=0, uid=b'\xff' * 6):
    return hdr(T_QUERY, sess) + bytes([what]) + uid

def info(uid, label, version, last_invalid, run_id, inv_id, web_ver, web_id, web_size, uptime,
         sess=0, state=0, ota=1, verify_left=0xFFFF, slot=0, flags=0x11, caps=0x3F, board=2,
         max_fw=0x4EF000, max_web=WEB_CAP, rb_cause=0, rb_detail=0):
    p = (hdr(T_INFO, sess) + bytes([1, 1, board, caps]) + uid +
         bytes([state, ota]) + struct.pack('<H', verify_left) + bytes([slot, flags]) +
         struct.pack('<II', max_fw, max_web) + bytes([CHUNK, WIN]) +
         s(version, 16) + s(last_invalid, 16) + s(label, 16) +
         run_id + inv_id + bytes([rb_cause, rb_detail]) +
         s(web_ver, 16) + web_id + struct.pack('<II', web_size, uptime))
    assert len(p) == 129, len(p)
    return p

def offer(sess, target, board, enc, flags, img_size, stream_size, sha, uid, version, sender, kind,
          est_s, stream_crc, comp_sha=bytes(32), comp_size=0):
    p = (hdr(T_OFFER, sess) + bytes([target, board, enc, flags, CHUNK]) +
         struct.pack('<II', img_size, stream_size) + sha + uid +
         s(version, 16) + s(sender, 16) + bytes([kind]) + struct.pack('<H', est_s) +
         struct.pack('<I', stream_crc) + comp_sha + struct.pack('<I', comp_size))
    assert len(p) == 131, len(p)
    return p

def reply(sess, decision, reason, secs, win):
    return hdr(T_REPLY, sess) + bytes([decision, reason]) + struct.pack('<H', secs) + bytes([win])

def data(sess, off, chunk):
    return MFG + bytes([T_DATA, sess]) + off.to_bytes(3, 'little') + chunk

def ack(sess, state, base, end, bitmap, crc, hold):
    return hdr(T_ACK, sess) + bytes([state]) + struct.pack('<IIQIH', base, end, bitmap, crc, hold)

def status(sess, state, reason, base, ssize, written, isize, secs):
    return hdr(T_STATUS, sess) + bytes([state, reason]) + struct.pack('<IIIIH', base, ssize, written, isize, secs)

def result(sess, code, phase, detail, sha):
    return hdr(T_RESULT, sess) + bytes([code, phase, detail]) + sha

def apply_(sess, mode):
    return hdr(T_APPLY, sess) + bytes([mode])

def abort(sess, reason):
    return hdr(T_ABORT, sess) + bytes([reason])

def identify(secs, uid):
    return hdr(T_IDENT, 0) + bytes([secs]) + uid

def descriptor(board, w, h, version, board_name, proto=1, flags=0x01):
    d = (b'NPFWDESC' + bytes([1, board]) + struct.pack('<HHH', 64, w, h) +
         bytes([proto, flags]) + b'\x00\x00' + s(version, 16) + s(board_name, 28))
    assert len(d) == 64
    return d

def image_id(img):
    """First 8 bytes of esp_app_desc_t.app_elf_sha256 (file offset 0x20 + 0x90 = 0xB0)."""
    assert img[0] == 0xE9 and struct.unpack_from('<I', img, 0x20)[0] == 0xABCD5432
    return img[0xB0:0xB8]

def hx(b):
    return ' '.join(f'{x:02X}' for x in b)

def show(name, payload, prio, src, dst, seq=0, max_frames=None):
    cid = can_id(prio, PGN, src, dst)
    frames = fp_frames(payload, seq)
    print(f"\n### {name}: {len(payload)} bytes, {len(frames)} frame(s), CAN-ID 0x{cid:08X}")
    print(f"payload: {hx(payload)}")
    for i, f in enumerate(frames):
        if max_frames is not None and max_frames <= i < len(frames) - 1:
            if i == max_frames:
                print("  ...")
            continue
        print(f"  frame {i:2d}: {hx(f)}   ({frame_bits(cid, f)} bits on the wire)")
    return frames

def vectors():
    SND, DEV = 0xE6, 0x2A
    uid = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x01])
    print("CAN-ID 126720 prio 7, dst 0x2A, src 0xE6 = 0x%08X" % can_id(7, PGN, SND, DEV))
    print("CAN-ID 126720 prio 6, dst 0xE6, src 0x2A = 0x%08X" % can_id(6, PGN, DEV, SND))
    print("CAN-ID 126720 prio 6, dst 0xFF, src 0xE6 = 0x%08X" % can_id(6, PGN, SND, 0xFF))
    print("crc32(b'123456789') = 0x%08X" % zlib.crc32(b'123456789'))
    show("QUERY(INFO) broadcast", query(1), 6, SND, 0xFF)
    show("QUERY(INFO) broadcast, unique id filter", query(1, uid=uid), 6, SND, 0xFF)
    show("QUERY(RESULT) session 0x5A", query(4, sess=0x5A), 6, SND, DEV)
    sha = hashlib.sha256(b'NauticPinnace N2K update test image').digest()
    wsha = hashlib.sha256(b'NauticPinnace N2K update test web UI').digest()
    print("\nsha256(b'NauticPinnace N2K update test image')  =", sha.hex())
    print("sha256(b'NauticPinnace N2K update test web UI') =", wsha.hex())
    run_id = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88])
    show("INFO", info(uid, "S/V Example", "1.3.0", "", run_id, bytes(8), "1.3.0", wsha[:8], 48751, 754),
         6, DEV, SND)
    scrc = zlib.crc32(b'NauticPinnace N2K update test stream')
    print("\ncrc32(b'NauticPinnace N2K update test stream') = 0x%08X" % scrc)
    show("OFFER firmware with companion web UI",
         offer(0x5A, 1, 2, 1, 0x00, 2441792, 1313452, sha, uid, "1.3.0", "PC Updater", 2, 360,
               scrc, wsha, 48751), 6, SND, DEV)
    show("OFFER_REPLY pending", reply(0x5A, 0, 0, 60, 0), 6, DEV, SND)
    show("OFFER_REPLY accepted", reply(0x5A, 1, 0, 0, 64), 6, DEV, SND)
    stream = bytes((i & 0xFF) for i in range(CHUNK * WIN * 2))
    show("DATA chunk 0", data(0x5A, 0, stream[:CHUNK]), 7, SND, DEV, seq=0, max_frames=2)
    show("DATA chunk 1", data(0x5A, CHUNK, stream[CHUNK:2 * CHUNK]), 7, SND, DEV, seq=1, max_frames=2)
    show("ACK first credit", ack(0x5A, 3, 0, CHUNK * WIN, 0, 0, 0), 6, DEV, SND)
    bm = ((1 << WIN) - 1) & ~(1 << 5) & ~(1 << 17)
    show("ACK with holes (chunks 5 and 17 missing)", ack(0x5A, 3, 0, CHUNK * WIN, bm, 0, 0), 6, DEV, SND)
    c1 = zlib.crc32(stream[:CHUNK * WIN])
    print("\ncrc32(stream[0:13824]) with stream[i] = i & 0xFF = 0x%08X" % c1)
    show("ACK window 1 complete, processing", ack(0x5A, 4, CHUNK * WIN, CHUNK * WIN, 0, c1, 400), 6, DEV, SND)
    show("ACK window 2 credit", ack(0x5A, 3, CHUNK * WIN, 2 * CHUNK * WIN, 0, c1, 0), 6, DEV, SND)
    show("STATUS verifying", status(0x5A, 5, 0, 1313452, 1313452, 2441792, 2441792, 30), 6, DEV, SND)
    show("RESULT verify ok", result(0x5A, 0, 1, 0, sha), 6, DEV, SND)
    show("APPLY now", apply_(0x5A, 0), 6, SND, DEV)
    show("ABORT user cancelled on device", abort(0x5A, 0x17), 6, DEV, SND)
    show("IDENTIFY 10 s", identify(10, uid), 6, SND, DEV)
    d = descriptor(2, 1024, 600, "1.3.0", "waveshare-7b-1024x600")
    print("\n### firmware descriptor at image offset 0x120 (7B, 1.3.0)\n" + hx(d))
    plain = b'NauticPinnace inflate self-test ' * 2
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    st = co.compress(plain) + co.flush()
    print(f"\n### inflate self-test: raw deflate ({len(st)} B) of b'NauticPinnace inflate self-test ' * 2 "
          f"({len(plain)} B)\n{hx(st)}")
    print("sha256(b'abc') =", hashlib.sha256(b'abc').hexdigest())
    print("\nframe bits, all-zero data, ID 0x1DEF2AE6:", frame_bits(0x1DEF2AE6, bytes(8)))
    print("frame bits, all-0xFF data, ID 0x1DEF2AE6:", frame_bits(0x1DEF2AE6, b'\xff' * 8))

def model(path):
    img = open(path, 'rb').read()
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    raw15 = co.compress(img) + co.flush()
    SND, DEV = 0xE6, 0x2A
    did = can_id(7, PGN, SND, DEV)
    print(f"\n{path}\n  image {len(img)} B, deflate w15 {len(raw15)} B ({len(raw15)/len(img):.1%}), "
          f"stream bound {len(img) + len(img) // 1000 + 1024} B")
    if img[:1] == b'\xe9':
        print(f"  image id (app_elf_sha256[0..7] at 0xB0): {hx(image_id(img))}")
    for label, stream in (("raw", img), ("deflate", raw15)):
        bits = 0
        nfr = 0
        for k, off in enumerate(range(0, len(stream), CHUNK)):
            for fr in fp_frames(data(0x5A, off, stream[off:off + CHUNK]), k & 7):
                bits += frame_bits(did, fr)
                nfr += 1
        nwin = -(-len(stream) // (CHUNK * WIN))
        ackbits = 2 * nwin * 5 * 160
        mean = bits / nfr
        for share in (0.3, 0.5):
            t_exact = bits / (BITRATE * share)
            t_cons = nfr * 160 / (BITRATE * share)
            print(f"  {label:<8} share {share:.0%}: stream {len(stream)} B, DATA frames {nfr}, mean {mean:5.1f} "
                  f"bit/frame, windows {nwin}, sender time exact {t_exact:6.1f} s = {t_exact/60:4.1f} min / "
                  f"160 bit {t_cons:6.1f} s = {t_cons/60:4.1f} min; device ACK traffic {ackbits/1000:.0f} kbit")

def main():
    vectors()
    for p in sys.argv[1:]:
        model(p)

if __name__ == "__main__":
    main()
