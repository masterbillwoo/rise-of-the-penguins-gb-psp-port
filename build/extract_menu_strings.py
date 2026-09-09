#!/usr/bin/env python3
"""Pull every translatable string out of psp/gamemenu.c.

The menu translates by English source text (see psp/menutext.h), so this list IS
the set of lookup keys - a translation whose key does not match byte for byte
falls back to English silently. Regenerate after touching any menu text:

    python build/extract_menu_strings.py > build/menu_strings.txt
"""
import io, os, re, sys

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "_masterboy", "psp", "gamemenu.c")

LIT = r'\"((?:[^\"\\]|\\.)*)\"'

# Arrays whose contents reach the screen through Tr()
NAME_ARRAYS = [
    "onOff", "filterNames", "bezelNames", "bezelDescs", "filterDescs",
    "scalingNames", "rateNames", "stereoNames", "boostNames", "fpsNames",
    "gbTypeNames", "resumeNames", "presetNames", "presetDescs",
]

def literals(text):
    return re.findall(LIT, text)

def main():
    src = io.open(SRC, encoding="latin-1").read()
    found = []

    # explicit Tr("...") call sites
    found += re.findall(r'Tr\(\s*' + LIT, src)

    # value-name and description arrays
    for name in NAME_ARRAYS:
        m = re.search(r'static const char \*%s\[\]\s*=\s*\{(.*?)\};' % name, src, re.S)
        if m:
            found += literals(m.group(1))

    # ITEM tables: row labels and their one-line descriptions
    for m in re.finditer(r'static const ITEM \w+\[\]\s*=\s*\{(.*?)\n\};', src, re.S):
        found += literals(m.group(1))

    # page titles
    m = re.search(r'static const PAGE pages\[P_COUNT\]\s*=\s*\{(.*?)\};', src, re.S)
    if m:
        found += literals(m.group(1))

    # first-run wizard steps
    m = re.search(r'static const STEP onboardSteps\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
    if m:
        found += literals(m.group(1))

    # labels in the button-layout diagram
    for m in re.finditer(r'KeyRow\([^;]*?' + LIT, src):
        found.append(m.group(1))

    seen, out = set(), []
    for s in found:
        if s and s not in seen and not s.endswith(".ini") and not s.endswith(".png"):
            seen.add(s)
            out.append(s)

    out.sort()
    for s in out:
        sys.stdout.write(s + "\n")
    sys.stderr.write("%d translatable strings\n" % len(out))

main()
