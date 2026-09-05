# -*- coding: utf-8 -*-
"""Build NVS partition images in the real on-flash format, with invented
values, so the salvage test has something to read that is not a dump of
somebody's device.

The layout is the one nvscheck.c's code under test walks: 4 KB pages, a
32-byte header, a two-bit-per-entry state bitmap, then 126 thirty-two-byte
entries. A namespace entry (type 0x01, ns 0) maps a name to an index; a
blob-data entry (type 0x42) carries its length in the first two data bytes
and its payload in the entries that follow.

Cross-checked against real dumps: the same binary reads a genuine stock
partition and a genuine converted one identically. Those dumps are not in
this repository -- they hold Wi-Fi passwords, a printer serial and an access
code -- which is exactly why these are generated instead.
"""
import struct, sys, os

PAGE, HDR, BITMAP, ENTRY = 4096, 32, 32, 32
ENTRIES = (PAGE - HDR - BITMAP) // ENTRY          # 126

def page(entries):
    """entries: list of (ns, type, span, key, data32) -- data32 is 32 bytes."""
    buf = bytearray(b'\xff' * PAGE)
    struct.pack_into('<I', buf, 0, 0xFFFFFFFE)    # written, not freed
    struct.pack_into('<I', buf, 4, 0)             # sequence
    buf[8] = 0xFE                                 # version 2
    bm = bytearray(b'\xff' * BITMAP)
    slot = 0
    for ns, ty, span, key, data in entries:
        e = bytearray(b'\x00' * ENTRY)
        e[0], e[1], e[2], e[3] = ns, ty, span, 0xFF
        k = key.encode()[:15]
        e[8:8 + len(k)] = k
        e[24:32] = data[:8]
        buf[HDR + BITMAP + slot * ENTRY: HDR + BITMAP + (slot + 1) * ENTRY] = e
        for i in range(span):                     # 2 = written
            b, sh = (slot + i) // 4, ((slot + i) % 4) * 2
            bm[b] = (bm[b] & ~(3 << sh)) | (2 << sh)
        # the payload rides in the entries the span covers
        if len(data) > 8:
            off = HDR + BITMAP + (slot + 1) * ENTRY
            buf[off:off + len(data) - 8] = data[8:]
        slot += span
    buf[HDR:HDR + BITMAP] = bm
    return bytes(buf)

def wifi_info(ssid, password, ap_ssid, ap_pw):
    """stock's blob: ssid[33], password[64], ap_ssid[33], ap_password[...]"""
    b = bytearray(228)
    b[0:33]    = ssid.encode().ljust(33, b'\0')
    b[33:97]   = password.encode().ljust(64, b'\0')
    b[97:130]  = ap_ssid.encode().ljust(33, b'\0')
    b[130:194] = ap_pw.encode().ljust(64, b'\0')
    return bytes(b)

def stock_image(ssid, password):
    blob = wifi_info(ssid, password, 'Panda_Vent_001122334455', '987654321')
    span = 1 + (len(blob) + ENTRY - 1) // ENTRY
    ns  = (0, 0x01, 1, 'app_nvs', bytes([1]) + b'\0' * 7)
    dat = (1, 0x42, span, 'wifi_info', struct.pack('<H', len(blob)) + b'\0' * 6 + blob)
    return page([ns, dat]) + b'\xff' * (PAGE * 2)

def converted_image():
    """A vent already running this firmware: no app_nvs at all."""
    ns = (0, 0x01, 1, 'pv', bytes([1]) + b'\0' * 7)
    return page([ns]) + b'\xff' * (PAGE * 2)

if __name__ == '__main__':
    d = sys.argv[1] if len(sys.argv) > 1 else 'images'
    os.makedirs(d, exist_ok=True)
    out = {
        'nvs-stock.bin':           stock_image('', ''),                       # never provisioned
        'nvs-stock-populated.bin': stock_image('example-iot', 'not-a-real-password'),
        'nvs-ours.bin':            converted_image(),
        'nvs-blank.bin':           b'\xff' * (PAGE * 3),
    }
    for name, data in out.items():
        open(os.path.join(d, name), 'wb').write(data)
        print('  %s  %d bytes' % (name, len(data)))
