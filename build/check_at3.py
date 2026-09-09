#!/usr/bin/env python3
"""Report whether an .AT3 is in the format the PSP XMB actually plays.

Ground truth came from MasterBoy's own shipping EBOOT, whose SND0.AT3 the XMB does
play: WAVE format tag 0x0270 (plain ATRAC3), 44100 Hz, 66 kbps, block align 192.
ATRAC3plus (tag 0xFFFE) files decode fine in players but the XMB stays silent.
"""

import struct
import sys

GOOD_TAG = 0x0270		# WAVE_FORMAT_SONY_SCX (ATRAC3)
PLUS_TAG = 0xFFFE		# WAVE_FORMAT_EXTENSIBLE (ATRAC3plus)
SIZE_LIMIT = 500 * 1024


def describe(path):
    d = open(path, "rb").read()
    if len(d) < 20 or d[:4] != b"RIFF":
        return None, "not a RIFF file"

    i, info = 12, {}
    while i < len(d) - 8:
        cid = d[i:i + 4]
        sz = struct.unpack_from("<I", d, i + 4)[0]
        if cid == b"fmt ":
            tag, ch, rate, bps, align, bits = struct.unpack_from("<HHIIHH", d, i + 8)
            info = dict(tag=tag, ch=ch, rate=rate, kbps=bps * 8 // 1000, align=align)
        if cid == b"data":
            break
        i += 8 + sz + (sz & 1)
    info["size"] = len(d)
    return info, None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build/config/SND0.AT3"
    info, err = describe(path)
    if err:
        print("%s: %s" % (path, err))
        return 1

    print("%s" % path)
    print("  format tag  0x%04X %s" % (info["tag"],
          "(ATRAC3)" if info["tag"] == GOOD_TAG else
          "(ATRAC3plus)" if info["tag"] == PLUS_TAG else "(unknown)"))
    print("  rate        %d Hz" % info["rate"])
    print("  bitrate     %d kbps" % info["kbps"])
    print("  block align %d" % info["align"])
    print("  size        %d bytes" % info["size"])

    ok = True
    if info["tag"] != GOOD_TAG:
        print("\n  XMB WILL BE SILENT: needs tag 0x0270 (plain ATRAC3), not this.")
        print("  The ATRACTool-embedded encoder is the PS3 build and can only emit")
        print("  ATRAC3plus. Encoding this needs Sony's PSP at3tool:")
        print("      at3tool -e -br 66 build/audio/SND0_source.wav SND0.AT3")
        print("  (build/audio/SND0_source.wav is already 44.1kHz 16-bit for it.)")
        ok = False
    if info["rate"] != 44100:
        print("  Note: PSP SND0.AT3 is 44100 Hz; this is %d." % info["rate"])
        ok = False
    if info["size"] > SIZE_LIMIT:
        print("  Too big: %d bytes, limit is ~500KB (shared with ICON1.PMF)."
              % info["size"])
        ok = False

    if ok:
        print("\n  Looks right for the XMB.")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
