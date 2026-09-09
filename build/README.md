# Single-game GB/GBC launcher — MasterBoy build

Boots straight from the XMB into one `.gb`/`.gbc` ROM, the way
TempGBA4PSP-Single-game does for GBA. Built on MasterBoy (`_masterboy/`), the only
GB/GBC emulator here with usable source.

## Build

Needs Docker running.

```sh
bash build/docker-build.sh      # image + compile -> build/out/EBOOT.PBP
python build/package.py         # -> dist/<one folder per ROM>
```

Copy each `dist/<folder>` into `ms0:/PSP/GAME/` on the PSP.

`package.py --title "Some Name"` changes the base name shown in the XMB.

## How the single-game mode works

`SingleGameFindRom()` in [psp/menuplus.c](../_masterboy/psp/menuplus.c) scans the
`roms/` folder next to the EBOOT and returns the first `.gb`/`.gbc`/`.sms`/`.gg`
file. `menuPlusShowMenu()` calls it once, on first boot only, *after* all its
first-time initialisation (user config, skins, fonts, menu state) but *before* the
menu loop.

This placement matters. `menuPlusShowMenu()` is not just "draw a menu" — it also
runs `LoadUserDefaultConfig()`, `menuLoadFiles()` and `InitMenus()`. Replacing the
call outright (rather than short-circuiting its loop) would boot the emulator with
hardcoded defaults and an uninitialised in-game menu, breaking save states and
options.

`MenuPlusAction(MA_LOADROM, …)` sets `menuPlusTerminated`, so the menu loop never
iterates and control falls through to the function's normal clean-up. If the ROM
fails to load, `MenuPlusAction` returns 0, `menuPlusTerminated` stays clear, and the
browser appears as usual — so a bad ROM degrades to stock behaviour instead of a
black screen. Pressing HOME in-game returns to a fully working menu, because the
first-boot flag has been cleared by then.

The ROM path is absolute, derived from `argv[0]` in
[psp/main.c](../_masterboy/psp/main.c) (`gblAppPath`). It has to be: `sceIoChdir()`
only runs under `KERNEL_MODE`, which is off, and save paths are derived from the ROM
path (`pspGetStateNameEx` builds `<romdir>/SAVE/<name>.sav.gz`). A relative path
would scatter saves depending on the thread cwd.

Because ROMs keep their original filenames and saves sit beside them, the ten
language variants never share a save file.

## Changes to _masterboy

Single-game feature:

- `psp/main.c` — `main()` now takes `argc`/`argv`; `InitAppPath()` fills `gblAppPath`.
- `psp/menuplus.c` — `SingleGameFindRom()` plus the first-boot hook in
  `menuPlusShowMenu()`; menu music is suppressed when booting straight into a game.
- `psp/menuplus.h` — declarations.
- `psp/VideoGu.c` — OSLib splash screens commented out.

The hook deliberately mirrors the ROM browser's post-load sequence, not just its
`MenuPlusAction(MA_LOADROM, ...)` call. The browser also runs
`LoadDefaultMachineConfig()`, loads the per-game `.ini`, and then issues
`gb_reset()` + `machine_manage_sram(SRAM_LOAD, 0)`. That SRAM_LOAD is the *only*
place a battery save is read, so a hook that skipped it would boot the game with an
empty save every time. The browser gates this on `gblFlagLoadParams`, which it sets
itself; we never enter it, so we always load with parameters.

Fixes needed to compile a 2008 codebase against PSPSDK 15.2 / OSLib MOD 1.5:

- `psp/usb.h`, `psp/usb.c`, `Makefile.psp` — MasterBoy shipped its own USB storage
  shim (`oslInitUsbStorage` etc.) because the OSLib of the day had none. Current
  OSLib provides that API in `<oslib/usb.h>`, so the private copy collided with it.
  The shim is dropped and `psp/usb.o` removed from `OBJS`; OSLib's version is used.
- `psp/menuplus.c` — `pspTime` → `ScePspDateTime`, `.minutes` → `.minute`.
- `Makefile.psp` — added `-ljpeg`, which current OSLib needs.

Note `Makefile.psp` has no default goal of its own (the per-file rules sit above the
`build.mak` include, so `cpu/z80.o` would otherwise win). Always build the explicit
`all` target.

## Compatibility with this game

Verified against the ROM headers rather than assumed:

| Field | Value | Handled by |
|---|---|---|
| `0x143` CGB flag | `0xC0` GBC-only | `gbcore/rom.c:80` |
| `0x147` cart type | `0x1E` MBC5+RAM+Battery+Rumble | `gbcore/mbc.c` |
| `0x148` ROM size | `0x06` = 2 MB / 128 banks | `rom_size_tbl[6]` |
| `0x149` RAM size | `0x03` = 32 KB | `ram_size_tbl[3]`, `sram_space` is 128 KB |

Rumble is inert on PSP hardware. The ROM is `malloc`'d whole
(`loadrom.c:106`), so 2 MB is comfortable on a Slim/3000 and worth watching on a
Phat/1000.

## Shipped settings

`build/config/DEFAULT.INI` is copied over the stock one at package time. Two changes
from MasterBoy's defaults, both specific to running as a single-game launcher:

- `video.render` 3 -> 2. Mode 3 is "Wide", which stretches 160x144 across the full
  480px panel and visibly distorts the picture. Mode 2 is "Fit", a uniform
  `min(scaleX, scaleY)` scale (302x272, pillarboxed), so the aspect ratio is right.
- `video.cpuTime` 1 -> 0. Mode 1 painted a live CPU-usage percentage over the
  top-left of the game.

Everything else is stock, including `file.sramAutosave = true`.

### Two kinds of saving, which are easy to confuse

- **The game's own save** (`GAME.sav.gz`) - the cartridge battery save, what the
  game writes when the player saves in-game.
- **The quit snapshot** (`GAME.auto.gz`) - a picture of the whole machine, taken on
  exit, so the next launch carries on mid-level.

The menu originally called the first "Autosave" and the emulator calls it a battery
save; both read as though they meant the second. They are now **"Write game save"**,
**"Auto-write game save"** and **"Snapshot on quit"**, and every row on that page
carries a one-line description in the panel footer explaining what it does.

### The viewport is zero until the first frame

`SmsEmulate()` memsets `bitmap` and only `gbe_refresh()` - **inside** the emulation
loop - sets the viewport to 160x144. Anything drawn before the loop therefore sees
`viewport.w == 0`, and every branch of `VideoGuUpdate_Core` divides by or scales
against it. MIPS does not trap on integer divide-by-zero: "Fit" produced a garbage
scale and `pspScaleImage` wrote outside the frame buffer.

PPSSPP shrugged this off, so the start gate tested fine here and crashed on
hardware. `VideoGuUpdate_Core` now returns early on a zero viewport.

Worth remembering generally: **PPSSPP is more forgiving than the PSP**, so a clean
run in the emulator is not evidence that out-of-range drawing is safe.

### Snapshot crash-loop protection

A snapshot that crashes while being restored would crash on *every* launch from then
on, with no way back into the menu to delete it. A `.auto.gz.try` marker is written
just before the restore and removed just after; if it is still there at boot, the
previous attempt did not survive, so the snapshot is discarded and the game starts
clean.

### Input handover between menu and game

Two separate problems, both in how a press outlives the screen it was made on.

**The dismissing press reached the game.** `ControlsUpdate()` samples the pad, runs
the menu inline via `ControlsMenuUpdate()`, then maps the pad to game input. The
button that closed the menu is still physically held on the next frame, so closing
with Circle pressed B in-game. `gblSwallowInput` (Controls.c) now holds game input
back until the pad is released - armed at all three exits: the menu, the resume
prompt and the start gate.

It is a frame countdown, not just a "wait for release" flag. `menuKeysAnalogApply()`
folds the analog stick into the button word, so a stick resting off-centre keeps
setting direction bits and input would never be handed back. It gives up after two
seconds regardless.

**Auto-repeat applied to every key.** With Cross and Circle repeating, a press held
a moment too long walked several rows or popped straight out of the menu. Only the
d-pad repeats now (`osl_keys->autoRepeatMask`).

### Start gate

`GameMenuStartGate()` holds on the title before the first frame: the game's name,
either *"Ready to play"* or **"Continuing where you left off"**, and a blinking
*"Press any button to continue"*. It exists so a resume does not drop the player
into the middle of a level with no warning, and so there is some acknowledgement
that a snapshot was restored at all. `file.startPrompt` turns the hold off; the
resume line still shows when a snapshot was actually loaded.

### Auto save state and resume

`SmsTerm()` in [psp/SMS.c](../_masterboy/psp/SMS.c) writes `STATE_AUTO` before
powering off, so it covers quitting from the menu *and* HOME -> Quit Game, both of
which end the main loop. The menu's Quit no longer calls `machine_poweroff()`
itself; it just sets `osl_quit` and lets `SmsTerm()` do the state and the battery
save in the right order.

On launch, the single-game hook checks whether `GAME.auto.gz` exists and only sets
`gblPendingAutoLoad`. The load itself happens in `SmsEmulate()` after
`system_poweron()` - restoring a state into a half-initialised machine is asking for
trouble, so it waits until the emulator is actually up.

`file.autoStateLoad` controls what happens then:

| Value | Menu label | Behaviour |
|---|---|---|
| 0 | Never | ignore the state |
| 1 | Ask | `GameMenuAskResume()` offers Resume / New, defaulting to Resume |
| 2 | Always | resume silently **(shipped default)** |

*Delete resume state* on the Save/Load page removes the file.

`STATE_AUTO` is slot 12, given its own filename (`.auto.gz`) in `pspGetStateNameEx`,
so it can never collide with the player's slots 0-9.

**Sleep is not covered.** The PSP's power switch suspends rather than exits, so
`SmsTerm()` never runs; MasterBoy's power callback in `psp/main.c` is commented out
upstream. Quitting is handled, sleeping is not.

### Battery saves are now flushed on a timer

Stock MasterBoy writes the battery save **only** on power-off, on loading another
ROM, or from the menu's manual command - and its power-switch callback in
`psp/main.c` is commented out. A crash, a flat battery or a yanked Memory Stick
therefore lost everything since boot.

`machine_sram_autosave_tick()` in [psp/SMS.c](../_masterboy/psp/SMS.c) is called
once per emulated frame and flushes every `SRAM_AUTOSAVE_FRAMES` (600, about ten
seconds - see `psp/SMS.h`). `machine_manage_sram` compares a CRC first, so a tick
with nothing changed costs one pass over the SRAM and no I/O at all. The flush sets
`gblSramSilentSave` so it never raises the low-battery modal mid-game.

The OSLib splash screens in `VideoGuInit()` are commented out — splash 2 is the
Neoflash logo, splash 1 the OSLib logo. Neither belongs in front of a game.

### Controls, as shipped

Game pad: D-pad as expected, **A = Cross**, **B = Circle**, Start, Select.
**Square and Triangle are autofire A and B** (`ctrl.keys` auto1/auto2, rate
`ctrl.autofireRate = 2`). Fast-forward and slow motion remain on R+Square and
R+Triangle; all of it is remappable from the Controls menu.

Shortcuts (`ctrl.shortcuts`):

| Combo | Action |
|---|---|
| L | Open menu |
| R + Select | Save state |
| R + Start | Load state |
| R + Up / R + Down | Next / previous state slot |
| Square | Turbo fire A (autofire) |
| Triangle | Turbo fire B (autofire) |
| R + Square | Fast-forward |
| R + Triangle | Slow motion |
| R + Cross | Pause |
| R + Circle | Screenshot |
| Select (menu only) | Music player |
| — | Reset is unassigned by default |

Battery saves (SRAM) are written automatically. Save states are separate and manual.

To change any of this per language, edit `build/config/DEFAULT.INI` and re-run
`package.py`. MasterBoy also reads a per-machine `default_gbc.ini` and a per-game
`roms/SAVE/<romname>.ini` if you want overrides that do not affect every build.

## The in-game menu

Pressing **L** opens our own menu, not MasterBoy's carousel. It lives in
[psp/gamemenu.c](../_masterboy/psp/gamemenu.c) as a self-contained module, hooked in
at a single line in `ControlsShowMenu()` (psp/Controls.c). `menuplus.c` is untouched
by it - that file is ~5000 lines of interdependent state and editing it in place had
already produced one hard crash, so the new menu shares nothing with it.

`menuPlusShowMenu()` is still called once at boot, because that is where the
emulator loads DEFAULT.INI, fonts and skins. It returns immediately in single-game
mode, and the player never sees it.

Design: a vertical list in the game's own four-colour DMG palette (224/248/208,
136/192/112, 52/104/86, 8/24/32 - taken from the sprite art) over the paused game,
which stays visible but dimmed. The cursor is the "hard hat penguin" sprite from the
original GB Studio project, its transparency key (101,255,0) stripped, shipped as
`menu/cursor.png`.

### Two upstream bugs made "revert to default" meaningless

`InitConfig()` in menuplus.c allocates `menuConfigDefault` but **never writes to
it** - so every revert read uninitialised heap. And `LoadUserDefaultConfig()` did
`memcpy(dst, &menuConfig, sizeof(&menuConfigUserDefault))`, where `sizeof(&ptr)` is
4 bytes, so the shipped DEFAULT.INI values were never recorded either. MasterBoy's
own "Reload defaults" was broken the same way.

Both are fixed: the built-in defaults are captured at the end of `InitConfig()`, and
the user defaults copy `sizeof(MENUPARAMS)`.

The menu reverts to **`menuConfigUserDefault`** - the launcher's own DEFAULT.INI -
not MasterBoy's factory settings. "Default" should mean what the launcher boots
with, not Wide scaling with the CPU meter on.

### Button layout page

**Controls > Button layout** draws the pad instead of listing it: shell outline,
shoulders, d-pad, the four face buttons in their real positions, Start/Select, and a
two-column key of what each one currently does. Faster to read than stepping through
the remap rows.

An earlier version drew leader lines from each control to its label; at 480x272 the
lines crossed the text and made both harder to read, so the key sits below the
diagram instead.

### Pages

| Page | Contents |
|---|---|
| Main | Resume, Video, Audio, Controls, Save/Load, Reset all settings, Restart, Quit |
| Video | Screen filter, Screen size, Smoothing, Gamma, Colour vibrance, Show framerate, Frameskip |
| Audio | Sound, Volume, Volume boost, Sample rate, Output, Sound in turbo |
| Controls | Buttons >, Shortcuts >, Analog as D-pad, Analog deadzone, Autofire speed |
| Buttons | remap all 10 game buttons |
| Shortcuts | remap all 10 emulator shortcuts |
| Save / Load | State slot, Save state, Load state, Save now, Autosave |

Cross selects, Circle backs out, Left/Right change a value, **Square reverts that
one setting to its default**, and *Reset all settings* on the main page restores
everything (keeping the loaded ROM).

### The menu remembers where you were

Page, highlighted row, scroll position and the whole page stack are kept in statics
across openings, so closing the menu to look at a change and opening it again lands
back on the same row rather than at the top of the main page. The restored values
are range-checked on the way in, so nothing stale can point past the end of a page.

### Layout

The list **scrolls**: at most `VISIBLE_ROWS` (8) are drawn at a time, with `^`/`v`
markers. This is not cosmetic - the previous flat list of 11 rows ran from y=54 to
y=274 on a panel ending at 251, so the last rows drew straight over the footer hint.
Current geometry: panel x 72-408 of 480, y 18-254 of 272; rows occupy 54-200; the
footer separator is at 224 and its text at 232-246. 24px of clearance.

Note PPSSPP enforces a minimum window size and upscales the framebuffer, so
screenshots of it are cropped and are **not** 1:1 with PSP coordinates. Check
layout arithmetic against the constants above rather than against a screenshot.

### Menu sound

Three short square-wave blips (move / select / back) are synthesised at runtime in
`SfxBuild()` - no asset to ship and no file I/O. They play on their own hardware
channel reserved with `sceAudioChReserve`, so nothing is shared with the emulator's
audio thread, and the channel is released when the menu closes. They follow the
master volume and are silent when Sound is off.

### Master volume

`sound.volume` in MasterBoy is only the *music player's* boost, so a real control was
added: `sound.masterVolume` (0-100). It is applied in `psp/PspSound.c` where the
audio actually reaches the hardware, scaling `PSP_AUDIO_VOLUME_MAX` at the
`sceAudioOutputBlocking` call.

## Screen overlay (LCD grid / bezel)

MasterBoy had no overlay feature; one was added. `OverlayDraw()` in
[psp/VideoGu.c](../_masterboy/psp/VideoGu.c) composites a single 480x272 RGBA
`overlay.png` from the app directory over the finished frame, under the status text.
`video.overlay` selects which filter: 0 is off, 1-5 index the table below. The same
list is on **Video > Render > Overlay** in the menu, so filters can be switched
while the game runs, without a rebuild.

| Index | Menu label | File in `overlays/` |
|---|---|---|
| 0 | Off | - |
| 1 | LCD fine | `lcd_fine.png` (shipped default) |
| 2 | LCD coarse | `lcd_coarse.png` |
| 3 | Scanlines | `scanlines.png` |
| 4 | Recessed | `shadow_only.png` |
| 5 | Pixel grid | `pixel_aligned.png` |

The menu list and the `overlayFiles` table in `psp/VideoGu.c` are both **static and
must stay in step**. An earlier version scanned `overlays/` and built the menu array
at run time; that reliably crashed the menu system (jump to 0x20202020), so the list
is fixed. To restyle an entry, regenerate its PNG - do not rename files.

`OverlaySync()` does the reload, and is called *before* `oslStartDrawing()`. Loading
a PNG between `oslStartDrawing` and `oslEndDrawing` means file I/O and freeing an
image the GPU may still be reading from.

One image covers both jobs: semi-transparent grid lines over the play area, and
opaque bezel art in the pillarbox margins.

`build/make_overlay.py` generates it, replaying the same integer scaling maths as
`VideoGuUpdate_Core` so grid lines land on real Game Boy pixel boundaries:

```sh
python build/make_overlay.py                       # default, alpha 40
python build/make_overlay.py --grid-alpha 70
python build/make_overlay.py --bezel art.png       # bezel in the margins + grid
python build/make_overlay.py --mode 1x --bezel art.png
```

Pre-generated variants are in `build/overlays/`. To use one, copy it over
`build/config/overlay.png` and re-run `package.py`.

### Bezel and grid are two independent layers

They started as one list, which made them mutually exclusive for no reason - a
Game Boy shell around the screen and an LCD texture on the picture are different
things and the normal case is wanting both. There are now two settings, two file
tables and two images composited in `OverlayDraw()`:

| Setting | Folder | Options |
|---|---|---|
| `video.overlay` | `overlays/` | Off, LCD fine, LCD coarse, Scanlines, Recessed, Pixel grid |
| `video.bezel` | `bezels/` | Off, Game Boy Color, Game Boy, Game Boy Pocket |

Grid is drawn first, then the bezel over it: the shell is opaque outside the screen
and its inner shading should sit on top of the texture rather than under it.

`--bezel` takes art straight from [Watomsk/Overlays](https://github.com/Watomsk/Overlays).
Those files are 1334x750 with the artwork - a power LED, the console's name down one
side - drawn around a **transparent window**. Cover-fitting one to 480x272 would put
that window in the wrong place, so `bezel_window()` finds the hole and the bezel is
scaled and offset until the hole sits exactly on the play area. The aspect ratios
differ a little, so the surround stretches slightly; it is flat colour and lettering
at the edges and takes it fine. Bezels are generated with `--grid-alpha 0 --shadow 0`
so they stay a pure frame and compose with any grid.

**Both layers only line up in Screen size: Fit**, the geometry they are generated
for. Rather than force the scaling, the menu says so - pick Wide or Full with either
layer on and the description reads *"Only lines up with Screen size: Fit"*.

### Per-value descriptions

Most rows describe themselves, but for the overlay list what matters is what the
*selected entry* looks like, so `filterDescs[]` describes each choice as you cycle
it. `filterNames[]`, `filterDescs[]` and `overlayFiles[]` in VideoGu.c are three
parallel arrays and must stay in step.

### Monochrome and palettes

**Video > Game Boy** carries the machine type (Auto / Game Boy / Super GB / GB Color
/ GB Advance), the monochrome palette, and per-game colourisation.

Palette names are read straight out of `palettes.ini` - the `_Name:` lines - into a
plain array by `PaletteScan()`. The menu row stays static and only its displayed
value comes from that array; building menu items at run time is what crashed the
menu system earlier. Setting the name is enough: `MenuOptionsConfigure()` notices
the change on the way out of the menu and applies it.

Despite the ROM's CGB-only flag (`0x143 = 0xC0`), forcing **Game Boy** mode works -
the game renders in monochrome and the palettes recolour it. Verified: DMG mode
greyscale, then Red and Bright Blue applied both from the menu and from the config.

Two fixes were needed to get there:

- `config.c` listed `gameboy.palette` as `TYPE_CHAINE` with **no `TYPE_MAXSIZE`**.
  The loader does `maxSize = 1 << TYPE_MAXSIZE_MASK(type)`, so with no mask the
  name was truncated to a single byte: a palette could never be loaded from a
  config file at all, only picked live from the menu. The field is `char[32]`,
  hence `TYPE_MAXSIZE(5)`.
- `SmsEmulate()` re-applies the palette after the machine is up. The one
  `MenuOptionsConfigure(0)` does earlier does not survive the reset that follows.

### Grid styles, and why the obvious one looks wrong

`--style` picks how the grid is drawn:

| Style | What it does |
|---|---|
| `texture` (default) | fixed-pitch soft LCD texture; `--pitch` in screen px |
| `scanlines` | horizontal lines only, fixed pitch |
| `pixel` | lines locked to Game Boy pixel boundaries |

`pixel` sounds like the right answer and is not. At Fit scaling a Game Boy pixel is
**1.887 screen px**, so lines land on a 2px/2px/1px cadence - 142 cells of 2px and
18 of 1px across the play area - and that rounding reads as visible banding roughly
every 8 cells. A fixed 2px pitch has no such rounding, so it looks materially
cleaner even though it is not locked to the source pixels. It is a texture, not a
measurement, and at this scale a texture is what reads well.

Pre-generated in `build/overlays/`: `lcd_fine` (shipped default: texture, 2px,
alpha 55), `lcd_coarse` (3px, alpha 70), `scanlines`, `pixel_aligned`,
`shadow_only`. Copy one over `build/config/overlay.png` and re-run `package.py`.

### Why the reference packs cannot be used for the grid

Measured from `GBC/drkhrse/Perfect_GBC.png` in ourigen/perfect_overlays:

- 640x480 canvas, play area **507x480** = **3.17 screen px per source px**
- alpha modulates 51..193 (mean 120, ~47% average darkening), anti-aliased, with a
  ~10px repeat that resolves into RGB subpixel stripes

That effect needs roughly **3+ screen pixels per source pixel** to place one stripe
per subpixel. The PSP gives us **1.887** - about half. Downscaling the reference PNG
to 480x272 does not degrade gracefully; the stripe pattern aliases into heavy
vertical banding that wrecks the picture.

So: use those packs for **bezel art** via `--bezel`, and let this generator draw the
grid at PSP pitch. A faithful subpixel LCD simulation is not achievable on a 480x272
panel with a 160x144 source, and no overlay file can change that.

### Using the overlay packs

Neither [Watomsk/Overlays](https://github.com/Watomsk/Overlays) (1080p/720p) nor
[ourigen/perfect_overlays](https://github.com/ourigen/perfect_overlays) (640x480)
ships art at 480x272, and neither bakes in an LCD grid. Pass one to `--bezel` and
the generator cover-fits it to 480x272, punches the play area out so the game shows
through, and draws our own grid on top. Note that at Fit scaling the visible bezel
is only the two 89px side margins, so most of a 16:9 bezel design will be cropped
away - `--mode 1x` leaves far more room for the art.

## What is NOT available

There is no programmable shader support. Beyond the overlay added above, MasterBoy's
only image controls are the scaling modes, bilinear smoothing (`video.smoothing`),
gamma and colour vibrance. `psp/gameboy_render.c` contains an `x2 (scanline)` mode
inherited from RIN, but every call to its `render_screen()` is commented out in
`gbcore/gb.c` - dead code, not reachable from the UI.

For GB (non-colour) games there are palettes in `palettes.ini` and `Colorpak/`, but
this game is GBC-only so it supplies its own colours.

## XMB presentation

The EBOOT carries three XMB assets. The stock MasterBoy build fills all three with
its own branding; the packager replaces them:

| PBP section | Size | Source |
|---|---|---|
| ICON0.PNG | 144x80 | `build/config/ICON0.PNG` - the tile |
| PIC1.PNG | 480x272 | `build/config/PIC1.PNG` - the background |
| SND0.AT3 | - | emptied (see below) |

`build/make_xmb.py` derives both images from cover art. The tile *contains* the
whole cover on a blurred fill of itself so nothing is cropped; the background
*covers* the screen and is darkened to 55% so the XMB's white text stays readable.

```sh
python build/make_xmb.py --source build/art/cover.png
python build/make_xmb.py --source art.png --pic-source bg.png --darken 0.45
python build/package.py
```

Art currently used is `cover.png` from **rise-of-the-penguins-gb-portmaster-build**
(game 1). Do not use the `rise-of-the-penguins-2` repo's cover here - that is
*Penguin's Dark Ascent*, a different game.

Two options matter for these covers:

- `--anchor` (default 0.85) sets where the 480x272 background crops a 4:3 source
  vertically. The title logo sits low in this art, so a centred crop cuts it off;
  1.0 goes too far and pulls in the score/HUD bar.
- letterbox and pillarbox bars are stripped automatically (the PortMaster covers are
  640x480 canvases with the art inset). `--keep-bars` disables that.

### The XMB jingle

```sh
bash build/make_sound.sh "path/to/nintendo-game-boy-startup.wav"
python build/package.py
```

The XMB plays **plain ATRAC3, WAVE tag 0x0270, 44.1kHz, 66 kbps, block align 192**.
That is not a guess: it is what MasterBoy's own shipping EBOOT contains, and its
jingle does play on hardware. ATRAC3**plus** files decode fine in players and pass
an ffmpeg round-trip, but the XMB stays silent - which cost two device tests to
learn.

ffmpeg cannot encode ATRAC3, and Sony's encoder is not distributed on its own.
[ATRACTool-Reloaded](https://github.com/XyLe-GBP/ATRACTool-Reloaded) (MIT) ships
both Sony codec tools as ordinary files under `res/`, so the script fetches it and
uses **`res/psp_at3tool.exe`**.

The distinction matters. The older ATRACTool embeds only `ps3_at3tool`, which
refuses 44.1kHz input and has no 66 kbps mode - it physically cannot produce the
right file. `psp_at3tool` accepts the 44.1kHz WAV and reports
`Encoding 66 kbps (ATRAC3)`.

`build/check_at3.py` compares any `.AT3` against those numbers and says plainly
whether the XMB will play it. Drop a different encoder at `build/tools/at3tool.exe`
and the script prefers it.

`package.py --keep-sound` restores MasterBoy's original music; deleting
`build/config/SND0.AT3` leaves the tile silent.

### There is no Game Boy boot chime

The DMG/CGB startup logo and its "ba-ding" come from Nintendo's boot ROM, which
MasterBoy does not emulate: `gb_reset()` in `gbcore/gb.c` writes post-boot register
values directly (LCDC=0x91, BGP=0xFC) and jumps straight into the cartridge.
Reproducing it in-game would mean shipping Nintendo's copyrighted boot ROM, so the
game starts at its own title screen. The chime is instead used as the XMB hover
sound above, which needs no boot ROM. Note that the recording is Nintendo's audio:
fine for a personal build, but for anything public an original retro-style chime
would be the safer choice.

## Never pad a PBP

A PBP header stores only eight offsets. A section's **length is implied by the next
section's offset**, so padding inserted before a section is attributed to the
section before it.

Aligning SND0 to a 16-byte boundary - an attempt to match MasterBoy's, chasing the
XMB audio - therefore turned the empty `PIC0` into a 2-byte malformed PNG, and the
PSP rejected the whole EBOOT with **"corrupted data"**. Sections must be packed back
to back.

`verify_pbp()` now reads each EBOOT back after writing and refuses to ship if any
section differs by a single byte from what went in. This class of mistake resizes
sections silently rather than failing, and the first symptom is on the device.

## What each shipped folder contains

| Item | Why it is there |
|---|---|
| `EBOOT.PBP` | the emulator plus XMB art and title |
| `roms/` | the ROM, and `SAVE/` for battery saves and save states |
| `DEFAULT.INI` | settings (scaling, overlay, controls) |
| `overlay.png` | the LCD grid |
| `Res/` | fonts and icons for the in-game menu - **required** |
| `Skins/`, `Colorpak/`, `palettes.ini`, `plugins.ini` | optional extras |

`Skins/` (345KB) holds two alternative themes for MasterBoy's own menu, the one you
reach with L. `Colorpak/` and `palettes.ini` are DMG colourisation palettes, which a
GBC-only game never uses. None of them affect gameplay; they can be dropped from
`RESOURCES` in `package.py` to save about 400KB per folder.

## Testing status

Verified in PPSSPP 1.20.4 on the Japanese build: boots straight to the title screen
with no ROM browser, no splash screens, correct aspect ratio, and the XMB title
reads "Rise of the Penguins (Japanese)".

Not yet verified: real PSP hardware, battery-save restore across a reboot, and the
in-game menu (L) round trip.
