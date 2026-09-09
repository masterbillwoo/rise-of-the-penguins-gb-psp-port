#!/usr/bin/env python3
"""Package the patched MasterBoy EBOOT into one PSP launcher folder per ROM.

Each ROM in roms/ becomes a self-contained ms0:/PSP/GAME/<folder>/ directory that
boots straight into that ROM. Saves live in <folder>/roms/SAVE/, so the language
variants never share save files.

Usage (from the repo root):
    python build/package.py
    python build/package.py --title "Rise of the Penguins"
"""

import argparse
import os
import shutil
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EBOOT_IN = os.path.join(ROOT, "build", "out", "EBOOT.PBP")
ROMS_DIR = os.path.join(ROOT, "roms")
DIST_DIR = os.path.join(ROOT, "dist")
RESOURCE_SRC = os.path.join(ROOT, "_masterboy_release", "extracted", "Masterboy")

# Runtime files MasterBoy needs next to the EBOOT. Music/ is optional but small
# enough to keep, and the menu falls back gracefully if it is missing.
RESOURCES = ["Res", "Skins", "Colorpak", "plugins.ini", "palettes.ini"]

# Config files we ship in place of the stock ones (build/config/ overrides the
# copies in the MasterBoy release).
CONFIG_SRC = os.path.join(ROOT, "build", "config")
CONFIG_FILES = ["DEFAULT.INI"]

# Overlay filters, shipped as a folder the in-game menu can pick from
OVERLAY_SRC = os.path.join(ROOT, "build", "overlays")
BEZEL_SRC = os.path.join(ROOT, "build", "bezels")

ROM_EXTS = (".gb", ".gbc", ".sms", ".gg")

LANGUAGES = {
    "de": "German",
    "es": "Spanish",
    "fr": "French",
    "it": "Italian",
    "ja": "Japanese",
    "nl": "Dutch",
    "pl": "Polish",
    "pt": "Portuguese",
    "pt_BR": "Portuguese (BR)",
    "en": "English",
}

# --- PARAM.SFO ---------------------------------------------------------------

SFO_MAGIC = b"\x00PSF"
FMT_UTF8 = 0x0204
FMT_INT32 = 0x0404


def sfo_parse(blob):
    """Return an ordered list of (key, fmt, max_len, value) from a PARAM.SFO."""
    if blob[:4] != SFO_MAGIC:
        raise ValueError("not a PARAM.SFO")
    version, key_start, data_start, count = struct.unpack_from("<IIII", blob, 4)
    entries = []
    for i in range(count):
        off = 20 + i * 16
        key_off, fmt, data_len, data_max, data_off = struct.unpack_from("<HHIII", blob, off)
        key_abs = key_start + key_off
        key = blob[key_abs:blob.index(b"\x00", key_abs)].decode("utf-8")
        raw = blob[data_start + data_off:data_start + data_off + data_len]
        if fmt == FMT_INT32:
            value = struct.unpack("<I", raw)[0]
        else:
            value = raw.rstrip(b"\x00").decode("utf-8", "replace")
        entries.append([key, fmt, data_max, value])
    return version, entries


def sfo_build(version, entries):
    """Rebuild a PARAM.SFO from parsed entries, growing fields as needed."""
    entries = sorted(entries, key=lambda e: e[0])

    key_table = b""
    key_offsets = []
    for key, _fmt, _max, _val in entries:
        key_offsets.append(len(key_table))
        key_table += key.encode("utf-8") + b"\x00"
    while len(key_table) % 4:
        key_table += b"\x00"

    data_table = b""
    index = b""
    for (key, fmt, data_max, value), key_off in zip(entries, key_offsets):
        if fmt == FMT_INT32:
            raw = struct.pack("<I", int(value))
            data_len = 4
            data_max = 4
        else:
            raw = value.encode("utf-8") + b"\x00"
            data_len = len(raw)
            # Keep the original slot size when it still fits, otherwise grow it
            # (rounded up to 4) so the title is never truncated.
            if data_max < data_len:
                data_max = (data_len + 3) & ~3
            raw = raw + b"\x00" * (data_max - data_len)
        index += struct.pack("<HHIII", key_off, fmt, data_len, data_max, len(data_table))
        data_table += raw

    key_start = 20 + len(index)
    data_start = key_start + len(key_table)
    header = SFO_MAGIC + struct.pack("<IIII", version, key_start, data_start, len(entries))
    return header + index + key_table + data_table


# --- PBP ---------------------------------------------------------------------

PBP_MAGIC = b"\x00PBP"
PBP_SECTIONS = 8


def pbp_read(path):
    with open(path, "rb") as fh:
        blob = fh.read()
    if blob[:4] != PBP_MAGIC:
        raise ValueError("%s is not a PBP" % path)
    version = struct.unpack_from("<I", blob, 4)[0]
    offsets = list(struct.unpack_from("<%dI" % PBP_SECTIONS, blob, 8))
    bounds = offsets + [len(blob)]
    sections = [blob[bounds[i]:bounds[i + 1]] for i in range(PBP_SECTIONS)]
    return version, sections


def pbp_write(path, version, sections):
    """Write a PBP with the sections packed back to back.

    Do NOT pad between sections. A PBP stores only eight offsets: a section's length
    is whatever runs up to the *next* offset, so any padding is attributed to the
    section before it. Aligning SND0 to 16 bytes (to match MasterBoy's, chasing the
    XMB audio) turned the empty PIC0 into a 2-byte malformed PNG and the PSP
    reported the whole EBOOT as corrupted data.
    """
    header_len = 8 + PBP_SECTIONS * 4
    offsets = []
    cursor = header_len
    for sec in sections:
        offsets.append(cursor)
        cursor += len(sec)
    blob = PBP_MAGIC + struct.pack("<I", version)
    blob += struct.pack("<%dI" % PBP_SECTIONS, *offsets)
    blob += b"".join(sections)
    with open(path, "wb") as fh:
        fh.write(blob)


# PBP section indices
SEC_SFO, SEC_ICON0, SEC_ICON1, SEC_PIC0, SEC_PIC1, SEC_SND0, SEC_DATA, SEC_PSAR = range(8)


def build_eboot(src, dst, title, icon0=None, pic1=None, snd0=None, strip_snd=False):
    """Rewrite the EBOOT with a new title and, where supplied, new XMB art.

    The stock EBOOT carries MasterBoy's own icon, background and background music;
    replacing them is what makes the launcher look like the game rather than like
    the emulator it is built on.
    """
    version, sections = pbp_read(src)

    sfo_version, entries = sfo_parse(sections[SEC_SFO])
    found = False
    for entry in entries:
        if entry[0] == "TITLE":
            entry[3] = title
            found = True
    if not found:
        entries.append(["TITLE", FMT_UTF8, 0, title])
    sections[SEC_SFO] = sfo_build(sfo_version, entries)

    if icon0 and os.path.isfile(icon0):
        sections[SEC_ICON0] = open(icon0, "rb").read()
    if pic1 and os.path.isfile(pic1):
        sections[SEC_PIC1] = open(pic1, "rb").read()
    if snd0 and os.path.isfile(snd0):
        sections[SEC_SND0] = open(snd0, "rb").read()
    elif strip_snd:
        # No ATRAC3 encoder is available to make a game-appropriate jingle, and
        # MasterBoy's own menu music is worse than silence here.
        sections[SEC_SND0] = b""

    pbp_write(dst, version, sections)
    verify_pbp(dst, sections)


def verify_pbp(path, expected):
    """Read the PBP back and check every section survived byte for byte.

    Cheap insurance: a PBP's section lengths come from the neighbouring offsets, so
    a packing mistake silently resizes sections rather than failing, and the first
    symptom is the PSP saying "corrupted data".
    """
    _, got = pbp_read(path)
    names = ["PARAM.SFO", "ICON0.PNG", "ICON1.PMF", "PIC0.PNG",
             "PIC1.PNG", "SND0.AT3", "DATA.PSP", "DATA.PSAR"]
    for name, want, have in zip(names, expected, got):
        if want != have:
            raise SystemExit("%s: %s is %d bytes, expected %d - refusing to ship"
                             % (path, name, len(have), len(want)))


# --- packaging ---------------------------------------------------------------

def folder_and_title(rom_name, base_title):
    stem = os.path.splitext(rom_name)[0]
    lang_code = None
    if "_" in stem:
        suffix = stem.split("_", 1)[1]
        if suffix in LANGUAGES:
            lang_code = suffix
    if lang_code:
        title = "%s (%s)" % (base_title, LANGUAGES[lang_code])
        folder = "%s_%s" % (base_title.replace(" ", ""), lang_code)
    else:
        title = base_title
        folder = base_title.replace(" ", "")
    return folder, title


def icon_for(rom_name):
    """XMB icon for this build. The translations get one tagged with their
    language so ten launchers can be told apart; the English build is the plain
    one and keeps the untouched icon. Also falls back to it when
    build/make_icons.py has not been run."""
    plain = os.path.join(CONFIG_SRC, "ICON0.PNG")
    code = lang_of(rom_name)
    if not code:
        return plain
    tagged = os.path.join(CONFIG_SRC, "icons", "ICON0_%s.PNG" % code)
    return tagged if os.path.isfile(tagged) else plain


def lang_of(rom_name):
    """Language suffix of a ROM filename, or None for the plain English build."""
    stem = os.path.splitext(rom_name)[0]
    if "_" in stem:
        suffix = stem.split("_", 1)[1]
        if suffix in LANGUAGES:
            return suffix
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--title", default="Rise of the Penguins GB",
                    help="base title shown in the XMB")
    ap.add_argument("--eboot", default=EBOOT_IN)
    ap.add_argument("--out", default=DIST_DIR)
    ap.add_argument("--keep-sound", dest="strip_sound", action="store_false",
                    default=True,
                    help="keep MasterBoy's XMB background music instead of silence")
    args = ap.parse_args()

    if not os.path.isfile(args.eboot):
        sys.exit("EBOOT not found: %s\nBuild it first with build/docker-build.sh" % args.eboot)
    if not os.path.isdir(RESOURCE_SRC):
        sys.exit("MasterBoy resource files not found: %s" % RESOURCE_SRC)

    roms = sorted(f for f in os.listdir(ROMS_DIR) if f.lower().endswith(ROM_EXTS))
    if not roms:
        sys.exit("no ROMs found in %s" % ROMS_DIR)

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)

    for rom in roms:
        folder, title = folder_and_title(rom, args.title)
        dest = os.path.join(args.out, folder)
        os.makedirs(os.path.join(dest, "roms"))

        build_eboot(args.eboot, os.path.join(dest, "EBOOT.PBP"), title,
                    icon0=icon_for(rom),
                    pic1=os.path.join(CONFIG_SRC, "PIC1.PNG"),
                    snd0=os.path.join(CONFIG_SRC, "SND0.AT3"),
                    strip_snd=args.strip_sound)
        shutil.copy2(os.path.join(ROMS_DIR, rom), os.path.join(dest, "roms", rom))
        # Saves land in <romdir>/SAVE/ (see pspGetStateNameEx), so pre-create it.
        os.makedirs(os.path.join(dest, "roms", "SAVE"))

        for cfg in CONFIG_FILES:
            shutil.copy2(os.path.join(CONFIG_SRC, cfg), os.path.join(dest, cfg))

        # Menu art (penguin cursor), from the game's own sprites
        shutil.copytree(os.path.join(CONFIG_SRC, "menu"), os.path.join(dest, "menu"))

        os.makedirs(os.path.join(dest, "overlays"))
        for f in sorted(os.listdir(OVERLAY_SRC)):
            if f.lower().endswith(".png") and f != "PREVIEW.png":
                shutil.copy2(os.path.join(OVERLAY_SRC, f),
                             os.path.join(dest, "overlays", f))

        # Bezels are a separate layer from the LCD grid, so a separate folder
        os.makedirs(os.path.join(dest, "bezels"))
        for f in sorted(os.listdir(BEZEL_SRC)):
            if f.lower().endswith(".png"):
                shutil.copy2(os.path.join(BEZEL_SRC, f),
                             os.path.join(dest, "bezels", f))

        for res in RESOURCES:
            src = os.path.join(RESOURCE_SRC, res)
            if not os.path.exists(src):
                print("  warning: missing resource %s" % res)
                continue
            target = os.path.join(dest, res)
            if os.path.isdir(src):
                shutil.copytree(src, target)
            else:
                shutil.copy2(src, target)

        # Some languages need letters the stock 256-glyph font does not carry
        # (Polish has none of l-stroke, a-ogonek, s-acute...). Each build has its
        # own Res/, so those ship a font of their own rather than everyone paying
        # for glyphs they will never draw. See build/make_font_pl.py.
        code = lang_of(rom)
        langfont = os.path.join(CONFIG_SRC, "font_%s.oft" % (code or "").lower())
        if code and os.path.exists(langfont):
            shutil.copy2(langfont, os.path.join(dest, "Res", "font.oft"))
            print("  %-22s   + %s font" % ("", code))

        print("%-24s -> %s" % (folder, title))

    # Generate cleanly named zip archives in dist/releases ready for Itch.io / GitHub release
    releases_dir = os.path.join(args.out, "releases")
    os.makedirs(releases_dir, exist_ok=True)
    print("\nCreating release zip archives in %s..." % releases_dir)
    for rom in roms:
        folder, title = folder_and_title(rom, args.title)
        zip_base_name = "%s (PSP)" % title
        zip_path = os.path.join(releases_dir, zip_base_name)
        shutil.make_archive(zip_path, "zip", root_dir=args.out, base_dir=folder)
        print("  -> %s.zip" % zip_base_name)

    print("\n%d launcher(s) written to %s" % (len(roms), args.out))
    print("Release zips ready in %s" % releases_dir)
    print("Copy each folder into ms0:/PSP/GAME/ on your PSP.")


if __name__ == "__main__":
    main()
