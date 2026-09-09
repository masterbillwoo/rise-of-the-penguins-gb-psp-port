# Rise of the Penguins GB — PSP Edition

A standalone PlayStation Portable (PSP) release of **Rise of the Penguins GB**.

- **Auto-Resume & Autosave:** Picks up right where you left off when you quit or relaunch, with continuous background save protection.
- **Custom In-Game Menu (L Trigger):** Dedicated retro-styled settings menu with custom audio chirps and controller layout viewer.
- **Custom Bezels & Shaders:** Handheld console borders (adapted from Watomsk) and authentic LCD pixel grid filters.
- **Persistent Settings:** Video scaling, filters, volume, and control remaps save automatically.

---

## 🕹️ Controls

### In-Game (Default)
| PSP Button | Game Boy Function |
|---|---|
| **D-Pad / Analog Stick** | Directional Movement |
| **Cross (X)** | **A** (Jump / Action) |
| **Circle (O)** | **B** (Run / Cancel) |
| **Square ([])** | Turbo A |
| **Triangle (/\\)** | Turbo B |
| **Start** | Start (In-game pause / menu) |
| **Select** | Select |
| **L Trigger** | **In-Game Menu** (Display, Audio, Controls, Saves) |
| **HOME** | Return to PSP XMB |

### Shortcuts
| Combination | Action |
|---|---|
| **R + Select** | Quick Save State |
| **R + Start** | Quick Load State |
| **R + Up / Down** | Next / Previous Save Slot |
| **R + Square** | Fast Forward (Turbo) |
| **R + Triangle** | Slow Motion |
| **R + Cross** | Pause Emulation |
| **R + Circle** | Screenshot |

### In-Game Menu
- **D-Pad Up / Down:** Navigate options
- **D-Pad Left / Right:** Adjust option values
- **Cross (X):** Select / Confirm / Rebind Button
- **Circle (O):** Back / Close Menu
- **Square ([]):** Reset selected option to default
- **L Trigger:** Toggle Menu Closed

---

## 💾 Installation

### Physical PSP (PSP-1000 / 2000 / 3000 / Street / Go)
1. Ensure your PSP runs custom firmware (6.60 / 6.61 PRO or ME).
2. Connect your PSP via USB (or insert your Memory Stick into a PC).
3. Copy the `RiseofthePenguinsGB` folder from `dist/` into:
   ```
   ms0:/PSP/GAME/RiseofthePenguinsGB/
   ```
   *(For PSP Go internal storage, use `ef0:/PSP/GAME/RiseofthePenguinsGB/`)*
4. Launch the game from the PSP XMB **Game > Memory Stick** menu.

### PPSSPP Emulator
1. Copy the `RiseofthePenguinsGB` folder into your PPSSPP `memstick/PSP/GAME/` directory.
2. Launch PPSSPP and select the game from the list.

---

## 🛠️ Building From Source

### 1-Step Build via Docker (Recommended)
Requires Docker and Python 3:
```bash
bash build/docker-build.sh
python build/package.py
```
This builds the EBOOT binary inside the PSPSDK container and packages the standalone distribution bundle into `dist/RiseofthePenguinsGB/`.

---

## ⚖️ License & Credits

- **MasterBoy Core:** Licensed under the [GNU General Public License v2.0](LICENSE). Derived from MasterBoy 2.10 by Brunnis and upstream contributors.
- **Rise of the Penguins GB:** Original game assets and code copyright their respective creators.
- **Console Bezels & LCD Overlays:** Adapted from Watomsk's handheld overlay collection, tailored for PSP 480x272 display geometry.

