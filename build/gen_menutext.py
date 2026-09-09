RUNTIME = 'static const TRPAIR *activePairs = 0;\nstatic int activeCount = 0;\nstatic char activeCode[8] = "en";\n\nconst char *MenuStringsLanguage(void)\n{\n@treturn activeCode;\n}\n\n//"ms0:/PSP/GAME/RiseofthePenguinsGB_de/roms/game_de.gb" -> "de"\n//".../roms/game_pt_BR.gb" -> "pt_BR";  ".../roms/game.gb" -> nothing, so English.\nstatic int LanguageFromRom(const char *romPath, char *dst, int size)\n{\n@tstatic const char prefix[] = "game_";\n@tconst char *base, *p;\n@tint n = 0, i;\n\n@tdst[0] = 0;\n@tif (!romPath || !romPath[0])\n@t@treturn 0;\n\n@tbase = strrchr(romPath, \'/\');\n@tif (!base)\n@t@tbase = strrchr(romPath, \'\\\\\');\n@tbase = base ? base + 1 : romPath;\n\n@t//The PSP hands back 8.3 directory entries in upper case ("GAME_DE.GB"), so\n@t//match and extract without regard to case. Testing this on a PC, where the\n@t//name reads back as typed, is what hid it the first time.\n@tfor (i = 0; i < 5; i++)@t@t{\n@t@t char c = base[i];\n@t@t if (c >= \'A\' && c <= \'Z\')\n@t@t@t c += 32;\n@t@t if (c != prefix[i])\n@t@t@t return 0;\n@t}\n@tfor (p = base + 5; *p && *p != \'.\' && n < size - 1; p++)@t@t{\n@t@tchar c = *p;\n@t@tif (c >= \'A\' && c <= \'Z\')\n@t@t@tc += 32;\n@t@tdst[n++] = c;\n@t}\n@tdst[n] = 0;\n@treturn n > 0;\n}\n\n\nvoid MenuStringsInit(const char *romPath)\n{\n@tchar code[8];\n@tint i;\n\n@tif (!LanguageFromRom(romPath, code, sizeof(code)))\n@t@t//Nothing to go on: keep whatever was already chosen rather than\n@t@t//dropping back to English - this runs from every UI entry point.\n@t@treturn;\n\n@tfor (i = 0; languages[i].code; i++)@t@t{\n@t@tif (!strcmp(languages[i].code, code))@t@t{\n@t@t@tactivePairs = languages[i].pairs;\n@t@t@tactiveCount = languages[i].count;\n@t@t@tsafe_strcpy(activeCode, code, sizeof(activeCode));\n@t@t@treturn;\n@t@t}\n@t}\n@t//A build whose language has no table yet still runs, in English\n}\n\nconst char *Tr(const char *en)\n{\n@tint lo = 0, hi = activeCount - 1;\n\n@tif (!en || !activePairs)\n@t@treturn en;\n\n@twhile (lo <= hi)@t@t{\n@t@tint mid = (lo + hi) / 2;\n@t@tint c = strcmp(en, activePairs[mid].en);\n@t@tif (c == 0)\n@t@t@treturn activePairs[mid].tr;\n@t@tif (c < 0)\n@t@t@thi = mid - 1;\n@t@telse\n@t@t@tlo = mid + 1;\n@t}\n@t//Untranslated: the English reads correctly, which beats a blank row\n@treturn en;\n}'
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate psp/menutext.c from build/lang/<code>.json.

The menu looks translations up by their English source text, so the keys here must
match build/menu_strings.txt byte for byte. Anything that does not match simply
falls back to English, which is why this generator checks the keys rather than
trusting them. One file per language, named for the ROM suffix it matches.

The output is written as Latin-1 on purpose: the menu font is a 256-glyph byte
font, so an accented letter has to reach it as one Latin-1 byte. A translation
using a character outside Latin-1 is rejected here rather than rendering as junk
on the PSP.

    python build/gen_menutext.py
"""
import io, json, os, sys

BS = chr(92)
Q  = chr(34)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
LANGDIR = os.path.join(HERE, "lang")
KEYS = os.path.join(HERE, "menu_strings.txt")
OUT  = os.path.join(ROOT, "_masterboy", "psp", "menutext.c")
FONT = os.path.join(ROOT, "_masterboy", "Res", "font.oft")

#Widths the menu actually has, in pixels (see gamemenu.c):
#a row label shares its line with the value, a description gets the full panel.
LIMIT_LABEL = 200
LIMIT_LINE  = 300

#Most languages fit Latin-1, which is what the stock 256-glyph font holds. Polish
#does not - none of l-stroke, a-ogonek, s-acute are in Latin-1 - so it is encoded
#ISO-8859-2 and ships its own font built by build/make_font_pl.py. The generated .c
#is therefore written as raw bytes, since one file carries both encodings; C string
#literals are just bytes, so the compiler does not care.
ENCODINGS = {"pl": "iso-8859-2"}

#Japanese has no byte encoding that covers kana and kanji, so it uses a custom
#code page built alongside its font by build/make_font_ja.py.
CODEPAGE = os.path.join(HERE, "config", "font_ja_map.json")

def encoding_for(code):
    return ENCODINGS.get(code, "latin-1")

def custom_map(code):
    if code == "ja" and os.path.exists(CODEPAGE):
        return json.load(io.open(CODEPAGE, encoding="utf-8"))
    return None

def to_bytes(s, enc, cmap):
    """Encode one string for the font its language ships with."""
    if cmap is None:
        return s.encode(enc)
    out = bytearray()
    for ch in s:
        if ord(ch) < 0x80:
            out.append(ord(ch))
        else:
            out.append(cmap[ch])      # KeyError = the font builder is stale
    return bytes(out)

def font_widths(code=None):
    """Width table of the font this language will actually ship with."""
    path = os.path.join(HERE, "config", "font_%s.oft" % (code or "").lower())
    if not (code and os.path.exists(path)):
        path = FONT
    return open(path, "rb").read()[64:64 + 256]

def text_width(w, s, enc, cmap):
    return sum(w[b] for b in to_bytes(s, enc, cmap))

def cstr(s):
    return s.replace(BS, BS + BS).replace(Q, BS + Q)

def main():
    langs = {}
    for fn in sorted(os.listdir(LANGDIR)):
        if fn.endswith(".json"):
            langs[fn[:-5]] = json.load(io.open(os.path.join(LANGDIR, fn),
                                              encoding="utf-8"))
    keys  = set(io.open(KEYS, encoding="utf-8").read().split("\n"))
    problems = 0

    body = []
    for code in sorted(langs):
        pairs = langs[code]
        enc = encoding_for(code)
        cmap = custom_map(code)
        w = font_widths(code)
        for en in pairs:
            if en not in keys:
                sys.stderr.write("%s: key not in menu_strings.txt: %r\n" % (code, en))
                problems += 1
        rows = []
        for en in sorted(pairs):
            tr = pairs[en]
            try:
                to_bytes(tr, enc, cmap)
            except (UnicodeEncodeError, KeyError):
                sys.stderr.write("%s: not Latin-1, the font cannot draw it: %r\n" % (code, tr))
                problems += 1
                continue
            tw = text_width(w, tr, enc, cmap)
            if tw > LIMIT_LINE:
                sys.stderr.write("%s: %dpx, will not fit the panel: %r\n" % (code, tw, tr))
                problems += 1
            rows.append((en, tr))
        body.append((code, rows, enc, cmap))

    out = []
    def emit(line, enc="ascii"):
        out.append((line, enc))
    def emit_bytes(b):
        out.append((b, None))
    emit("//=== MENU TEXT ===")
    emit("//GENERATED by build/gen_menutext.py from build/lang/<code>.json.")
    emit("//Do not edit the tables by hand - edit the JSON and regenerate.")
    emit("//See menutext.h for why the English text is the key rather than a numeric id.")
    emit("//Byte encodings differ per table: Latin-1, except Polish which is")
    emit("//ISO-8859-2 and ships its own font. See build/gen_menutext.py.")
    emit("")
    emit('#include "pspcommon.h"')
    emit('#include "menutext.h"')
    emit("")
    emit("typedef struct { const char *en, *tr; } TRPAIR;")
    emit("typedef struct { const char *code; const TRPAIR *pairs; int count; } LANGUAGE;")
    emit("")
    for code, rows, enc, cmap in body:
        emit("//Sorted by en: Tr() binary searches these.")
        emit("static const TRPAIR lang_%s[] = {" % code)
        for en, tr in rows:
            emit_bytes(b'	{"' + cstr(en).encode("ascii") + b'", "'
                       + to_bytes(cstr(tr), enc, cmap) + b'"},')
        emit("};")
        emit("")
    emit("static const LANGUAGE languages[] = {")
    for code, rows, enc, cmap in body:
        emit('@t{"%s", lang_%s, %d},' % (code.lower(), code, len(rows)))
    emit("@t{0, 0, 0}")
    emit("};")
    emit("")
    emit(RUNTIME)

    #Assembled as bytes: each table is encoded for its own language, so one .c
    #file carries more than one encoding. C string literals are just bytes, so
    #the compiler is indifferent; the font is what has to agree.
    blob = b""
    for line, enc in out:
        if enc is None:
            blob += line + b"\n"
        else:
            blob += line.replace("@t", "\t").encode(enc) + b"\n"
    open(OUT, "wb").write(blob)
    for code, rows, enc, cmap in body:
        sys.stderr.write("%s: %d strings\n" % (code, len(rows)))
    if problems:
        sys.stderr.write("%d problem(s)\n" % problems)
        sys.exit(1)

main()
