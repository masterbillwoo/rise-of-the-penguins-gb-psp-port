//=== IN-GAME MENU ===
//See gamemenu.h. Drawn in the game's own four-colour Game Boy palette, as a
//vertical list, so that pressing L reads as part of the game rather than as
//MasterBoy's carousel.

#include <pspaudio.h>
#include <psppower.h>
#include "pspcommon.h"
#include "gamemenu.h"
#include "menutext.h"

//Rise of the Penguins GB's own four colours, lightest to darkest, taken from the
//game project's customColorsWhite/Light/Dark/Black (project/settings.gbsres). The
//game is authored in "mixed" colour mode, so these are what its DMG art is remapped
//to on Game Boy Color hardware - which is what the launcher runs it as, and so what
//the player is actually looking at. This used to be the generic DMG green every
//emulator uses, which is precisely why the menu felt like an emulator's.
#define GB_LIGHTEST	RGB(232, 248, 224)	//E8F8E0
#define GB_LIGHT	RGB(176, 240, 136)	//B0F088
#define GB_DARK		RGB(80, 152, 120)	//509878
#define GB_DARKEST	RGB(32, 40, 80)		//202850

#define PANEL_X		72
#define PANEL_W		336
#define PANEL_TOP	6
#define PANEL_BOT	268
#define ROW_H		19
#define ROW_TOP		56
//The frame art is 8px tiles drawn at 2x, so it eats 16px on every side. Content
//coordinates below are all clear of that band.
#define FRAME_T		16
#define TITLE_Y		28
#define TITLE_RULE	44
#define FOOT_RULE	(PANEL_BOT - 54)
//18px apart, not 14: the Japanese font's glyph cell is 16 rows (see
//build/make_font_ja.py) and at 14 the description and the hint below it touch.
#define FOOT_DESC	(PANEL_BOT - 50)
#define FOOT_HINT	(PANEL_BOT - 32)
//Rows visible at once; the list scrolls rather than running past the panel.
#define VISIBLE_ROWS	8

extern OSL_FONT *ftStandard;
extern int pspSaveState(int slot);
extern int pspLoadState(int slot);
//The launcher's own DEFAULT.INI, captured by LoadUserDefaultConfig(). This is
//what "default" should mean here - MasterBoy's factory settings (Wide scaling,
//CPU meter on, no overlay) are not what this launcher ships with.
extern MENUPARAMS *menuConfigUserDefault;
extern char *menuGetKeyName(char *dest, u32 maxLen, char *separator, u32 key);
extern void SaveUserDefaultConfig(void);
extern volatile int osl_vblCount;

static OSL_IMAGE *imgCursor = NULL;
static OSL_IMAGE *imgFrame = NULL;
static int cursorTried = 0;
static int stateSlot = 0;

//--- Game Boy palettes -------------------------------------------------------
//palettes.ini lists them as "_Name:" lines. They are read into a plain array
//rather than built into menu items - constructing menu structures at run time
//crashed the menu system once already, so the row stays static and only its
//displayed value comes from here.
#define MAX_PALETTES 32
#define MAX_PAL_NAME 28
static char paletteNames[MAX_PALETTES][MAX_PAL_NAME];
static int paletteCount = 0;
static int paletteIndex = 0;		//0 = the game's own colours

static void PaletteScan(void)
{
	char path[MAX_PATH], line[256];
	SceUID fd;
	int n = 0, pos = 0;
	char buf[1024];
	int got;

	paletteCount = 0;
	if (gblAppPath[0])
		snprintf(path, sizeof(path), "%s/palettes.ini", gblAppPath);
	else
		strcpy(path, "palettes.ini");

	fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
	if (fd < 0)
		return;

	while ((got = sceIoRead(fd, buf, sizeof(buf))) > 0)		{
		int i;
		for (i = 0; i < got; i++)		{
			char c = buf[i];
			if (c == '\n' || c == '\r')		{
				line[pos] = 0;
				//"_Name:" introduces a palette
				if (pos > 2 && line[0] == '_' && line[pos - 1] == ':' &&
				    paletteCount < MAX_PALETTES)		{
					int len = pos - 2;
					if (len > MAX_PAL_NAME - 1)
						len = MAX_PAL_NAME - 1;
					memcpy(paletteNames[paletteCount], line + 1, len);
					paletteNames[paletteCount][len] = 0;
					paletteCount++;
				}
				pos = 0;
			}
			else if (pos < (int)sizeof(line) - 1)
				line[pos++] = c;
		}
	}
	sceIoClose(fd);
	n = n;
}

//Match the saved palette name back to an index when the menu opens
static void PalettePull(void)
{
	int i;
	paletteIndex = 0;
	if (!menuConfig.gameboy.palette[0])
		return;
	for (i = 0; i < paletteCount; i++)
		if (!strcmp(paletteNames[i], menuConfig.gameboy.palette))		{
			paletteIndex = i + 1;
			return;
		}
}

static void PalettePush(void)
{
	if (paletteIndex <= 0 || paletteIndex > paletteCount)
		menuConfig.gameboy.palette[0] = 0;
	else
		safe_strcpy(menuConfig.gameboy.palette, paletteNames[paletteIndex - 1],
		            sizeof(menuConfig.gameboy.palette));
}

//--- sound effects -----------------------------------------------------------
//Synthesised rather than loaded: short square-wave blips in the spirit of the
//game's own audio, with no asset to ship and no file I/O. They go out on their own
//hardware channel, so nothing is shared with the emulator's audio thread.
#define SFX_SAMPLES	2816		//must be a multiple of 64
#define SFX_MOVE	0
#define SFX_SELECT	1
#define SFX_BACK	2
#define SFX_COUNT	3

static short sfxBuf[SFX_COUNT][SFX_SAMPLES * 2] __attribute__((aligned(64)));
static int sfxChannel = -1;

static void SfxBuild(int which, int period, int lengthSamples, int amplitude)
{
	int i;
	short *out = sfxBuf[which];

	for (i = 0; i < SFX_SAMPLES; i++)		{
		short v = 0;
		if (i < lengthSamples)		{
			//Linear decay, so the blip does not click when it stops
			int amp = amplitude - (amplitude * i) / lengthSamples;
			v = ((i / period) & 1) ? (short)amp : (short)(-amp);
		}
		out[i * 2] = v;
		out[i * 2 + 1] = v;
	}
}

static void SfxInit(void)
{
	static int built = 0;

	if (!built)		{
		built = 1;
		//Loud enough to hear over the game: these are ~1/3 of full scale. The
		//first version used ~10% and was inaudible on hardware.
		SfxBuild(SFX_MOVE,   38, 1100, 10000);
		SfxBuild(SFX_SELECT, 26, 2200, 12000);
		SfxBuild(SFX_BACK,   58, 1600,  9000);
	}

	//Reserve once and keep it. Releasing on close meant the channel was gone for
	//every opening after the first.
	if (sfxChannel < 0)
		sfxChannel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, SFX_SAMPLES,
		                               PSP_AUDIO_FORMAT_STEREO);
}

static void SfxPlay(int which)
{
	int vol;
	static int sfxLastTick = 0;
	int now = osl_vblCount;
	if (sfxChannel < 0 || !menuConfig.sound.enabled)
		return;
	if (which == SFX_MOVE && now == sfxLastTick)
		return;
	sfxLastTick = now;
	vol = menuConfig.sound.masterVolume;
	if (vol <= 0)
		return;
	if (vol > 100)
		vol = 100;
	//Non-blocking, so the menu keeps running while the blip plays
	sceAudioOutput(sfxChannel, (PSP_AUDIO_VOLUME_MAX * vol) / 100, sfxBuf[which]);
}

//--- menu model --------------------------------------------------------------

enum {
	K_LINK = 0,	//opens another page
	K_ENUM,		//int chosen from a name list
	K_RANGE,	//int between lo and hi
	K_ACTION,	//does something
	K_KEY,		//ctrl.akeys[arg]
	K_CUT,		//ctrl.acuts[arg]
	K_PALETTE,	//cycles the palettes.ini list
	K_PRESET	//cycles the whole-pad layout presets
};

enum {
	P_MAIN = 0, P_VIDEO, P_AUDIO, P_CONTROLS, P_BUTTONS, P_SHORTCUTS, P_SAVE,
	P_LAYOUT, P_GAMEBOY, P_COUNT
};

enum {
	A_RESUME = 1, A_SAVESTATE, A_LOADSTATE, A_SAVENOW, A_RESET, A_SLEEP,
	A_QUIT, A_DEFAULTS_ALL, A_DELETE_AUTO, A_BACK
};

typedef struct {
	const char *label;
	int kind;
	int *field;			//K_ENUM / K_RANGE target
	const char **names;	//K_ENUM labels
	int lo, hi, step;	//K_RANGE bounds; for K_ENUM, lo is the list length
	int arg;			//page for K_LINK, action for K_ACTION, index for K_KEY/K_CUT
	const char *desc;	//one line shown at the foot of the panel when selected
} ITEM;

static const char *onOff[] = {"Off", "On"};
//Must stay in step with overlayFiles[] in VideoGu.c
static const char *filterNames[] = {
	"Off", "LCD fine", "LCD coarse", "Scanlines", "Recessed", "Pixel grid"
};
static const char *bezelNames[] = {
	"Off", "Game Boy Color", "Game Boy", "Game Boy Pocket"
};
static const char *bezelDescs[] = {
	"No shell around the screen",
	"Game Boy Color shell, power LED and all",
	"Original Game Boy shell",
	"Game Boy Pocket shell"
};

//A row description says what the row is; for the filter list what matters is what
//the chosen entry looks like, so it is described per value instead.
static const char *filterDescs[] = {
	"No texture; plain picture",
	"Soft LCD dot texture, the subtle one",
	"Chunkier LCD dots, more visible",
	"Horizontal lines only, no vertical grid",
	"Just a recessed screen edge, no dots",
	"Grid locked to GB pixels; can look banded"
};
static const char *scalingNames[] = {"1x", "Fit", "2x", "Wide", "Full", "1.5x"};
static const char *rateNames[] = {"11 kHz", "22 kHz", "44 kHz"};
static const char *stereoNames[] = {"Stereo", "Mono"};
static const char *boostNames[] = {"Normal", "Loud", "Loudest"};
static const char *fpsNames[] = {"Off", "CPU use", "Framerate"};
static const char *gbTypeNames[] = {"Auto", "Game Boy", "Super GB", "GB Color"};

//menuConfig.video.render is not a 0..n index (see menuMainVideoScalingItems in
//menuplus.c), so the menu edits a shadow index and maps it back.
static int scalingIndexShadow;

//--- button layout presets ---------------------------------------------------
//Two ways round, and both are defensible: Cross is what a PSP asks you to press to
//confirm, Circle is where A physically sits on a Game Boy. Rather than pick for the
//player, offer them as one row - rebinding ten keys by hand to swap A and B is a
//chore nobody should have to do.
#define PSPK_SELECT		0x0001
#define PSPK_START		0x0008
#define PSPK_UP			0x0010
#define PSPK_RIGHT		0x0020
#define PSPK_DOWN		0x0040
#define PSPK_LEFT		0x0080
#define PSPK_TRIANGLE	0x1000
#define PSPK_CIRCLE		0x2000
#define PSPK_CROSS		0x4000
#define PSPK_SQUARE		0x8000

//Order matches the akeys union in menuplus.h:
//up, down, left, right, button1, button2, start, auto1, auto2, select
//
//Those last names are Master System names, and they do NOT line up with the Game
//Boy's A and B. gbe_updatePad() in gameboy_render.c sets the GB's B bit from
//button1 and its A bit from button2 - the core's own comment says so. So in Game
//Boy terms the slots read:
//    button1 = B     button2 = A     auto1 = Turbo B     auto2 = Turbo A
//Get this backwards and the menu confidently tells you the opposite of what the
//pad does, which is exactly what it used to do.
#define PRESET_KEYS 10
#define PRESET_COUNT 2

static const u32 presetKeys[PRESET_COUNT][PRESET_KEYS] = {
	//Sony - X is A, O is B
	{PSPK_UP, PSPK_DOWN, PSPK_LEFT, PSPK_RIGHT, PSPK_CIRCLE, PSPK_CROSS,
	 PSPK_START, PSPK_TRIANGLE, PSPK_SQUARE, PSPK_SELECT},
	//Game Boy - O is A, X is B, as on the handheld
	{PSPK_UP, PSPK_DOWN, PSPK_LEFT, PSPK_RIGHT, PSPK_CROSS, PSPK_CIRCLE,
	 PSPK_START, PSPK_SQUARE, PSPK_TRIANGLE, PSPK_SELECT},
};

//Index 0 is not a preset: it is what the row shows once the bindings have been
//edited by hand and no longer match either table.
static const char *presetNames[] = {"Custom", "Sony", "Game Boy"};
static const char *presetDescs[] = {
	"Your own bindings, set on the Buttons page",
	"X is A, O is B - the PSP way round",
	"O is A, X is B - as on the handheld"
};
static int presetIndex = 1;

//Which preset the current bindings are, or 0 for none
static void PresetPull(void)
{
	int p, i;
	presetIndex = 0;
	for (p = 0; p < PRESET_COUNT; p++)		{
		for (i = 0; i < PRESET_KEYS; i++)
			if (menuConfig.ctrl.akeys[i] != presetKeys[p][i])
				break;
		if (i == PRESET_KEYS)		{
			presetIndex = p + 1;
			return;
		}
	}
}

static void PresetApply(int which)
{
	int i;
	if (which < 1 || which > PRESET_COUNT)
		return;
	for (i = 0; i < PRESET_KEYS; i++)
		menuConfig.ctrl.akeys[i] = presetKeys[which - 1][i];
	presetIndex = which;
}

//What the physical button does in the game right now, for the layout diagram.
//Derived from the bindings rather than assumed, so the picture stays honest after
//a preset swap or a hand edit.
static const char *FaceLabel(u32 key)
{
	//See presetKeys above: button1 is the GB's B, button2 its A
	if (menuConfig.ctrl.keys.button2 & key)	return "A";
	if (menuConfig.ctrl.keys.button1 & key)	return "B";
	if (menuConfig.ctrl.keys.auto2 & key)	return "TA";
	if (menuConfig.ctrl.keys.auto1 & key)	return "TB";
	if (menuConfig.ctrl.keys.start & key)	return "St";
	if (menuConfig.ctrl.keys.select & key)	return "Se";
	return "";
}

static const ITEM pageMain[] = {
	{"Resume",             K_ACTION, 0, 0, 0, 0, 0, A_RESUME},
	{"Video",              K_LINK,   0, 0, 0, 0, 0, P_VIDEO},
	{"Audio",              K_LINK,   0, 0, 0, 0, 0, P_AUDIO},
	{"Controls",           K_LINK,   0, 0, 0, 0, 0, P_CONTROLS},
	{"Save / Load",        K_LINK,   0, 0, 0, 0, 0, P_SAVE},
	{"Reset all settings", K_ACTION, 0, 0, 0, 0, 0, A_DEFAULTS_ALL,
	 "Put every option back to how this launcher ships"},
	{"Restart game",       K_ACTION, 0, 0, 0, 0, 0, A_RESET},
	{"Sleep",              K_ACTION, 0, 0, 0, 0, 0, A_SLEEP,
	 "Put the PSP into sleep mode"},
	{"Quit to XMB",        K_ACTION, 0, 0, 0, 0, 0, A_QUIT},
};

static const ITEM pageVideo[] = {
	{"LCD grid",        K_ENUM,  &menuConfig.video.overlay,   filterNames, 6, 0, 0, 0,
	 "Texture drawn over the picture"},
	{"Bezel",           K_ENUM,  &menuConfig.video.bezel,     bezelNames, 4, 0, 0, 0,
	 "Console shell drawn around the screen"},
	{"Screen size",     K_ENUM,  &scalingIndexShadow,         scalingNames, 6, 0, 0, 0,
	 "Fit keeps the shape right; Wide and Full stretch it"},
	{"Smoothing",       K_ENUM,  &menuConfig.video.smoothing, onOff, 2, 0, 0, 0},
	{"Gamma",           K_RANGE, &menuConfig.video.gamma,     0, 80, 200, 5, 0},
	{"Colour vibrance", K_RANGE, &menuConfig.video.vibrance,  0, 0, 256, 8, 0},
	{"Show framerate",  K_ENUM,  &menuConfig.video.cpuTime,   fpsNames, 3, 0, 0, 0},
	{"Frameskip",       K_RANGE, &menuConfig.video.frameskip, 0, 0, 10, 1, 0},
	{"Game Boy",        K_LINK,  0, 0, 0, 0, 0, P_GAMEBOY,
	 "Machine type and monochrome palettes"},
};

static const ITEM pageAudio[] = {
	{"Sound",          K_ENUM,  &menuConfig.sound.enabled,      onOff, 2, 0, 0, 0},
	{"Volume",         K_RANGE, &menuConfig.sound.masterVolume, 0, 0, 100, 5, 0,
	 "Output level of the game audio"},
	{"Volume boost",   K_ENUM,  &menuConfig.sound.volume,       boostNames, 3, 0, 0, 0,
	 "Extra gain for the built-in music player only"},
	{"Sample rate",    K_ENUM,  0,                              rateNames, 3, 0, 0, 0},
	{"Output",         K_ENUM,  &menuConfig.sound.stereo,       stereoNames, 2, 0, 0, 0},
	{"Sound in turbo", K_ENUM,  &menuConfig.sound.turboSound,   onOff, 2, 0, 0, 0},
};

static const ITEM pageControls[] = {
	{"Layout preset",   K_PRESET, 0, 0, 0, 0, 0, 0},
	{"Button layout",   K_LINK,  0, 0, 0, 0, 0, P_LAYOUT,
	 "See what every button currently does"},
	{"Buttons",         K_LINK,  0, 0, 0, 0, 0, P_BUTTONS},
	{"Shortcuts",       K_LINK,  0, 0, 0, 0, 0, P_SHORTCUTS},
	{"Analog as D-pad", K_ENUM,  &menuConfig.ctrl.analog.toPad,    onOff, 2, 0, 0, 0},
	{"Analog deadzone", K_RANGE, &menuConfig.ctrl.analog.treshold, 0, 1, 127, 4, 0,
	 "How far the stick must move before it counts"},
	{"Autofire speed",  K_RANGE, &menuConfig.ctrl.autofireRate,    0, 1, 10, 1, 0},
};

//Indices into the akeys union in menuplus.h. A and B are deliberately crossed over
//relative to the union's field order - see the note above presetKeys: slot 4
//(button1) drives the Game Boy's B and slot 5 (button2) drives its A. Labelling
//them in union order is what made the menu disagree with the game.
static const ITEM pageButtons[] = {
	{"Up",      K_KEY, 0, 0, 0, 0, 0, 0},
	{"Down",    K_KEY, 0, 0, 0, 0, 0, 1},
	{"Left",    K_KEY, 0, 0, 0, 0, 0, 2},
	{"Right",   K_KEY, 0, 0, 0, 0, 0, 3},
	{"A",       K_KEY, 0, 0, 0, 0, 0, 5},
	{"B",       K_KEY, 0, 0, 0, 0, 0, 4},
	{"Start",   K_KEY, 0, 0, 0, 0, 0, 6},
	{"Turbo A", K_KEY, 0, 0, 0, 0, 0, 8},
	{"Turbo B", K_KEY, 0, 0, 0, 0, 0, 7},
	{"Select",  K_KEY, 0, 0, 0, 0, 0, 9},
};

//Order matches the acuts union in menuplus.h
static const ITEM pageShortcuts[] = {
	{"Open menu",    K_CUT, 0, 0, 0, 0, 0, 0},
	{"Fast forward", K_CUT, 0, 0, 0, 0, 0, 1},
	{"Slow motion",  K_CUT, 0, 0, 0, 0, 0, 2},
	{"Pause",        K_CUT, 0, 0, 0, 0, 0, 3},
	{"Load state",   K_CUT, 0, 0, 0, 0, 0, 4},
	{"Save state",   K_CUT, 0, 0, 0, 0, 0, 5},
	{"Next slot",    K_CUT, 0, 0, 0, 0, 0, 6},
	{"Prev slot",    K_CUT, 0, 0, 0, 0, 0, 7},
	{"Reset",        K_CUT, 0, 0, 0, 0, 0, 8},
	{"Screenshot",   K_CUT, 0, 0, 0, 0, 0, 10},
};

//"Battery save" is the cartridge's own save, written by the game itself. The auto
//state is a full snapshot of the machine, taken when quitting. They are unrelated,
//and the old menu called only the first one "Autosave", which was misleading.
static const char *resumeNames[] = {"Never", "Ask", "Always"};

static const ITEM pageSave[] = {
	{"State slot", K_RANGE,  &stateSlot, 0, 0, 9, 1, 0,
	 "Which of the 10 snapshot slots Save/Load state uses"},
	{"Save state", K_ACTION, 0, 0, 0, 0, 0, A_SAVESTATE,
	 "Snapshot this exact moment into the slot above"},
	{"Load state", K_ACTION, 0, 0, 0, 0, 0, A_LOADSTATE,
	 "Jump back to the snapshot in the slot above"},
	{"Write game save", K_ACTION, 0, 0, 0, 0, 0, A_SAVENOW,
	 "Flush the game's own save file to the Memory Stick now"},
	{"Auto-write game save", K_ENUM, &menuConfig.file.sramAutosave, onOff, 2, 0, 0, 0,
	 "Keep the game's own save written every few seconds"},
	{"Snapshot on quit", K_ENUM, &menuConfig.file.autoState, onOff, 2, 0, 0, 0,
	 "On exit, snapshot where you were so you can carry on"},
	{"Resume on launch", K_ENUM, &menuConfig.file.autoStateLoad, resumeNames, 3, 0, 0, 0,
	 "What to do with that snapshot next time you start"},
	{"Start prompt", K_ENUM, &menuConfig.file.startPrompt, onOff, 2, 0, 0, 0,
	 "Wait for a button press before play begins"},
	{"Delete snapshot", K_ACTION, 0, 0, 0, 0, 0, A_DELETE_AUTO,
	 "Throw away the quit snapshot; next launch starts fresh"},
};

//The layout page draws a diagram instead of a list, so it needs no real items.
//A_BACK rather than A_RESUME: on a page that is just a picture, the obvious key
//should step back to Controls, not close the whole menu.
static const ITEM pageGameBoy[] = {
	{"Machine type", K_ENUM, &menuConfig.gameboy.gbType, gbTypeNames, 4, 0, 0, 0,
	 "Restart game to switch emulated hardware"},
	{"Palette", K_PALETTE, 0, 0, 0, 0, 0, 0,
	 "Monochrome colour scheme, from palettes.ini"},
	{"Colourise", K_ENUM, &menuConfig.gameboy.colorization, onOff, 2, 0, 0, 0,
	 "Per-game colouring for monochrome titles"},
};

static const ITEM pageLayout[] = {
	{"", K_ACTION, 0, 0, 0, 0, 0, A_BACK, 0},
};

typedef struct { const char *title; const ITEM *items; int count; } PAGE;

static const PAGE pages[P_COUNT] = {
	{"RISE OF THE PENGUINS GB", pageMain,      numberof(pageMain)},
	{"VIDEO",                pageVideo,     numberof(pageVideo)},
	{"AUDIO",                pageAudio,     numberof(pageAudio)},
	{"CONTROLS",             pageControls,  numberof(pageControls)},
	{"BUTTONS",              pageButtons,   numberof(pageButtons)},
	{"SHORTCUTS",            pageShortcuts, numberof(pageShortcuts)},
	{"SAVE / LOAD",          pageSave,      numberof(pageSave)},
	{"BUTTON LAYOUT",        pageLayout,    numberof(pageLayout)},
	{"GAME BOY",             pageGameBoy,   numberof(pageGameBoy)},
};

//--- scaling shim ------------------------------------------------------------
//video.render values, in the order scalingNames lists them
static const int scalingValues[] = {0, 2, 1, 3, 4, 5};

static void ScalingPull(void)
{
	int i;
	scalingIndexShadow = 1;
	for (i = 0; i < 6; i++)
		if (scalingValues[i] == menuConfig.video.render)
			scalingIndexShadow = i;
}

static void ScalingPush(void)
{
	if (scalingIndexShadow >= 0 && scalingIndexShadow < 6)
		menuConfig.video.render = scalingValues[scalingIndexShadow];
}

//--- defaults ----------------------------------------------------------------
//menuConfigUserDefault holds the built-in configuration. Any int in menuConfig maps to
//its default by the same byte offset, which is how menuplus.c reaches them too.
static int DefaultOf(const int *field)
{
	u32 offset = (u32)field - (u32)&menuConfig;
	return *(int*)((u32)menuConfigUserDefault + offset);
}

static void ResetItem(const ITEM *it)
{
	switch (it->kind)		{
		case K_ENUM:
		case K_RANGE:
			if (it->field == &scalingIndexShadow)		{
				menuConfig.video.render = menuConfigUserDefault->video.render;
				ScalingPull();
			}
			else if (it->field == &stateSlot)
				stateSlot = 0;
			else if (!it->field)
				menuConfig.sound.sampleRate = menuConfigUserDefault->sound.sampleRate;
			else
				*it->field = DefaultOf(it->field);
			break;
		case K_PALETTE:
			paletteIndex = 0;
			PalettePush();
			break;
		case K_PRESET:
			//Back to the shipped layout, not to "Custom" - Custom is a state the
			//bindings can be in, not something you can choose.
			PresetApply(1);
			break;
		case K_KEY:
			menuConfig.ctrl.akeys[it->arg] = menuConfigUserDefault->ctrl.akeys[it->arg];
			break;
		case K_CUT:
			menuConfig.ctrl.acuts[it->arg] = menuConfigUserDefault->ctrl.acuts[it->arg];
			break;
	}
}

//--- helpers -----------------------------------------------------------------

static void LoadCursor(void)
{
	char path[MAX_PATH];
	if (cursorTried)
		return;
	cursorTried = 1;
	if (gblAppPath[0])
		snprintf(path, sizeof(path), "%s/menu/cursor.png", gblAppPath);
	else
		strcpy(path, "menu/cursor.png");
	imgCursor = oslLoadImageFilePNG(path, OSL_IN_RAM, OSL_PF_8888);

	if (gblAppPath[0])
		snprintf(path, sizeof(path), "%s/menu/frame.png", gblAppPath);
	else
		strcpy(path, "menu/frame.png");
	imgFrame = oslLoadImageFilePNG(path, OSL_IN_RAM, OSL_PF_8888);
}

//--- panel border ------------------------------------------------------------
//The game's own dialogue frame (assets/ui/frame.png in the game project), nine-
//sliced. Drawn at 2x so one of its pixels is the size of one of the game's pixels
//on a 480x272 screen - at 1x the braid reads as a hairline and stops looking like
//anything the game would draw.
static void FrameTile(int sx, int sy, int dx, int dy)
{
	oslSetImageTileSize(imgFrame, sx, sy, 8, 8);
	imgFrame->stretchX = FRAME_T;
	imgFrame->stretchY = FRAME_T;
	oslDrawImageXY(imgFrame, dx, dy);
}

static void DrawFrame(int x0, int y0, int x1, int y1)
{
	int x, y;

	if (!imgFrame)		{
		//The plain rules the menu used before the art was available
		MyDrawFillRect(x0, y0, x0 + 3, y1, GB_LIGHT);
		MyDrawFillRect(x0, y0, x1, y0 + 1, GB_DARK);
		MyDrawFillRect(x0, y1 - 1, x1, y1, GB_DARK);
		return;
	}

	//Edges, then a tile flush to the far end in case the span is not a whole number
	//of tiles, then the corners over the lot
	for (x = x0 + FRAME_T; x < x1 - FRAME_T; x += FRAME_T)		{
		FrameTile(8, 0,  x, y0);
		FrameTile(8, 16, x, y1 - FRAME_T);
	}
	FrameTile(8, 0,  x1 - 2 * FRAME_T, y0);
	FrameTile(8, 16, x1 - 2 * FRAME_T, y1 - FRAME_T);

	for (y = y0 + FRAME_T; y < y1 - FRAME_T; y += FRAME_T)		{
		FrameTile(0,  8, x0, y);
		FrameTile(16, 8, x1 - FRAME_T, y);
	}
	FrameTile(0,  8, x0, y1 - 2 * FRAME_T);
	FrameTile(16, 8, x1 - FRAME_T, y1 - 2 * FRAME_T);

	FrameTile(0,  0,  x0, y0);
	FrameTile(16, 0,  x1 - FRAME_T, y0);
	FrameTile(0,  16, x0, y1 - FRAME_T);
	FrameTile(16, 16, x1 - FRAME_T, y1 - FRAME_T);
}

//Sample rate is stored as the rate itself, not as an index.
static const int rateValues[] = {11025, 22050, 44100};

static int RateIndex(void)
{
	int i;
	for (i = 0; i < 3; i++)
		if (rateValues[i] == menuConfig.sound.sampleRate)
			return i;
	return 2;
}

static void ItemValue(const ITEM *it, char *dst, int size)
{
	dst[0] = '\0';
	switch (it->kind)		{
		case K_LINK:
			strncpy(dst, ">", size - 1);
			break;
		case K_ENUM:
			if (!it->field)		//sample rate
				strncpy(dst, Tr(rateNames[RateIndex()]), size - 1);
			else if (*it->field >= 0 && *it->field < it->lo)
				strncpy(dst, Tr(it->names[*it->field]), size - 1);
			break;
		case K_RANGE:
			snprintf(dst, size, "%i", it->field ? *it->field : 0);
			break;
		case K_KEY:
			menuGetKeyName(dst, size, "/", menuConfig.ctrl.akeys[it->arg]);
			break;
		case K_CUT:
			menuGetKeyName(dst, size, "+", menuConfig.ctrl.acuts[it->arg]);
			break;
		case K_PALETTE:
			if (paletteIndex <= 0 || paletteIndex > paletteCount)
				strncpy(dst, Tr("Game's own"), size - 1);
			else
				strncpy(dst, paletteNames[paletteIndex - 1], size - 1);
			break;
		case K_PRESET:
			strncpy(dst, Tr(presetNames[(presetIndex >= 0 && presetIndex <= PRESET_COUNT)
			                            ? presetIndex : 0]), size - 1);
			break;
	}
	dst[size - 1] = '\0';
}

static void ItemAdjust(const ITEM *it, int dir)
{
	switch (it->kind)		{
		case K_ENUM:
			if (!it->field)		{
				int i = RateIndex() + dir;
				if (i < 0)
					i = 2;
				else if (i > 2)
					i = 0;
				menuConfig.sound.sampleRate = rateValues[i];
			}
			else		{
				*it->field += dir;
				if (*it->field < 0)
					*it->field = it->lo - 1;
				else if (*it->field >= it->lo)
					*it->field = 0;
			}
			break;
		case K_RANGE:
			if (it->field)		{
				*it->field += dir * it->step;
				if (*it->field < it->lo)
					*it->field = it->lo;
				else if (*it->field > it->hi)
					*it->field = it->hi;
			}
			break;
		case K_PALETTE:
			paletteIndex += dir;
			if (paletteIndex < 0)
				paletteIndex = paletteCount;
			else if (paletteIndex > paletteCount)
				paletteIndex = 0;
			PalettePush();
			break;
		case K_PRESET:		{
			//Custom is only ever arrived at by hand-editing a binding, so stepping
			//off it lands on a real preset and it is never stepped back onto.
			int next = presetIndex + dir;
			if (next < 1)
				next = PRESET_COUNT;
			else if (next > PRESET_COUNT)
				next = 1;
			PresetApply(next);
			break;
		}
	}
}

//--- key capture -------------------------------------------------------------
//MasterBoy's own routine (menuRedefineGetNewKey, menuplus.c) is tied to its window
//system, so the menu captures keys itself and draws the prompt in its own panel.
//The behaviour is modelled on it, with its two escape hatches kept: a timeout so a
//capture you did not mean to start always ends, and a commit on release so a tap
//counts. The first version of this had neither and could not be got out of.
#define STDKEYMASK 0xf80f3f9

//Bits 24-27 are the analog stick, which menuKeysAnalogApply folds into the button
//word. A stick resting off centre holds them set for good, so the old "wait until
//nothing is held" loops had no exit at all on a drifting pad - the menu simply
//stopped responding. Nothing here needs the stick as a binding, so it is left out.
#define CAPTUREMASK (STDKEYMASK & ~0x0f000000u)

//Frames to wait for the first press before giving up (60fps)
#define CAPTURE_TIMEOUT 360

static void DrawPage(int page, int sel, int scroll, const char *status,
                     const char *prompt);

static u32 CaptureKey(int page, int sel, int scroll, const char *label)
{
	char prompt[80], keyname[48];
	u32 got = 0, held;
	int frames;

	//Let go of whatever opened this first, but never wait forever
	for (frames = 0; !osl_quit && frames < 240; frames++)		{
		MyReadKeys();
		if (!(osl_keys->held.value & CAPTUREMASK))
			break;
		safe_strcpy(prompt, Tr("Let go first..."), sizeof(prompt));
		oslStartDrawing();
		DrawPage(page, sel, scroll, NULL, prompt);
		oslEndDrawing();
		oslSyncFrame();
	}

	//Everything held between the first press and the release is collected, so a
	//combination like R+Select does not have to land on a single frame, and a quick
	//tap is taken the moment it is let go. The old version wanted one unchanging
	//combination held for twenty straight frames, which threw taps away and made
	//two-button shortcuts almost impossible to enter.
	for (frames = 0; !osl_quit; frames++)		{
		MyReadKeys();
		held = osl_keys->held.value & CAPTUREMASK;
		got |= held;

		//Released - that is the binding
		if (got && !held)
			break;
		//Nothing at all pressed: give up rather than trapping the player here
		if (!got && frames >= CAPTURE_TIMEOUT)
			return 0;

		if (got)		{
			menuGetKeyName(keyname, sizeof(keyname), "+", got);
			snprintf(prompt, sizeof(prompt), Tr("%s   let go to set"), keyname);
		}
		else
			snprintf(prompt, sizeof(prompt), Tr("Press a button  (%is left)"),
			         (CAPTURE_TIMEOUT - frames + 59) / 60);

		oslStartDrawing();
		DrawPage(page, sel, scroll, NULL, prompt);
		oslEndDrawing();
		oslSyncFrame();
	}
	return got;
}

//--- drawing -----------------------------------------------------------------

//--- button layout diagram ---------------------------------------------------
//A drawn PSP rather than another list: the point is to see at a glance what every
//physical control does, which a column of names does not give you.

//Rounded-ish filled box - the PSP's shell and its screen are not hard rectangles,
//and square corners everywhere read as programmer art.
static void RoundRect(int x0, int y0, int x1, int y1, int rad, OSL_COLOR c)
{
	MyDrawFillRect(x0 + rad, y0, x1 - rad, y1, c);
	MyDrawFillRect(x0, y0 + rad, x0 + rad, y1 - rad, c);
	MyDrawFillRect(x1 - rad, y0 + rad, x1, y1 - rad, c);
	//Corner steps, cheap approximation of a radius at this size
	MyDrawFillRect(x0 + 1, y0 + 1, x0 + rad, y0 + rad, c);
	MyDrawFillRect(x1 - rad, y0 + 1, x1 - 1, y0 + rad, c);
	MyDrawFillRect(x0 + 1, y1 - rad, x0 + rad, y1 - 1, c);
	MyDrawFillRect(x1 - rad, y1 - rad, x1 - 1, y1 - 1, c);
}

//A control: filled key-cap with its symbol, highlighted if it does something
static void Cap(int cx, int cy, int w, int h, const char *sym, int active)
{
	OSL_COLOR body = active ? GB_LIGHT : GB_DARK;
	RoundRect(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, 3, body);
	oslSetTextColor(active ? GB_DARKEST : GB_LIGHTEST);
	oslDrawString(cx - GetStringWidth((char*)sym) / 2, cy - 6, (char*)sym);
}

//One "Name  binding" pair in the key below the diagram
static void KeyRow(int x, int y, const char *name, const char *bind)
{
	oslSetTextColor(GB_DARK);
	oslDrawString(x, y, (char*)Tr(name));
	oslSetTextColor(GB_LIGHTEST);
	oslDrawString(x + 62, y, (char*)bind);
}

static void DrawLayout(void)
{
	char a[32], b[32], ta[32], tb[32], st[32], se[32], mn[32];
	int bx0 = PANEL_X + 46, bx1 = PANEL_X + PANEL_W - 46;
	int by0 = 62, by1 = 152;
	int cx = bx0 + 42, cy = 112;			//d-pad centre
	int fx = bx1 - 42, fy = 112;			//face buttons centre
	int sp = 19, cw = 16;
	int col2 = PANEL_X + 176, row = 168;

	//Crossed over for the same reason as pageButtons: 4 is B, 5 is A
	menuGetKeyName(a,  sizeof(a),  "/", menuConfig.ctrl.akeys[5]);
	menuGetKeyName(b,  sizeof(b),  "/", menuConfig.ctrl.akeys[4]);
	menuGetKeyName(ta, sizeof(ta), "/", menuConfig.ctrl.akeys[8]);
	menuGetKeyName(tb, sizeof(tb), "/", menuConfig.ctrl.akeys[7]);
	menuGetKeyName(st, sizeof(st), "/", menuConfig.ctrl.akeys[6]);
	menuGetKeyName(se, sizeof(se), "/", menuConfig.ctrl.akeys[9]);
	menuGetKeyName(mn, sizeof(mn), "+", menuConfig.ctrl.cuts.menu);

	//Shell outline
	RoundRect(bx0, by0, bx1, by1, 8, GB_DARKEST);
	MyDrawFillRect(bx0, by0, bx1, by0 + 1, GB_DARK);
	MyDrawFillRect(bx0, by1 - 1, bx1, by1, GB_DARK);
	MyDrawFillRect(bx0, by0, bx0 + 1, by1, GB_DARK);
	MyDrawFillRect(bx1 - 1, by0, bx1, by1, GB_DARK);

	//Shoulders
	RoundRect(bx0 + 6, by0 - 8, bx0 + 48, by0 + 2, 3, GB_DARK);
	RoundRect(bx1 - 48, by0 - 8, bx1 - 6, by0 + 2, 3, GB_DARK);
	oslSetTextColor(GB_LIGHTEST);
	oslDrawString(bx0 + 22, by0 - 9, "L");
	oslDrawString(bx1 - 30, by0 - 9, "R");

	//Screen
	RoundRect(cx + 30, by0 + 16, fx - 30, by1 - 26, 2, GB_DARK);
	oslSetTextColor(GB_LIGHT);
	oslDrawString((cx + fx) / 2 - GetStringWidth((char*)Tr("GAME")) / 2, cy - 12,
	              (char*)Tr("GAME"));

	//D-pad
	Cap(cx, cy - sp, cw, cw, "", 1);
	Cap(cx, cy + sp, cw, cw, "", 1);
	Cap(cx - sp, cy, cw, cw, "", 1);
	Cap(cx + sp, cy, cw, cw, "", 1);
	Cap(cx, cy, cw, cw, "", 0);

	//Face buttons, in their real positions, each labelled with what it does in the
	//game rather than with its own name - the point of the picture is the mapping.
	{
		const char *lt = FaceLabel(PSPK_TRIANGLE), *lx = FaceLabel(PSPK_CROSS);
		const char *ls = FaceLabel(PSPK_SQUARE),   *lo = FaceLabel(PSPK_CIRCLE);
		Cap(fx, fy - sp, cw, cw, lt[0] ? lt : "T", lt[0] != 0);
		Cap(fx, fy + sp, cw, cw, lx[0] ? lx : "X", lx[0] != 0);
		Cap(fx - sp, fy, cw, cw, ls[0] ? ls : "S", ls[0] != 0);
		Cap(fx + sp, fy, cw, cw, lo[0] ? lo : "O", lo[0] != 0);
	}

	//Start / Select
	RoundRect(cx + 34, by1 - 20, cx + 70, by1 - 10, 2, GB_DARK);
	RoundRect(cx + 78, by1 - 20, cx + 114, by1 - 10, 2, GB_DARK);
	oslSetTextColor(GB_LIGHTEST);
	oslDrawString(cx + 38, by1 - 21, "SEL");
	oslDrawString(cx + 82, by1 - 21, "STA");

	//Key. Plain columns beat leader lines here - at this size the lines crossed the
	//labels and made both harder to read.
	MyDrawFillRect(PANEL_X + 18, row - 8, PANEL_X + PANEL_W - 18, row - 7, GB_DARK);
	KeyRow(PANEL_X + 18, row,      "A",       a);
	KeyRow(PANEL_X + 18, row + 16, "B",       b);
	KeyRow(PANEL_X + 18, row + 32, "Start",   st);
	KeyRow(PANEL_X + 18, row + 48, "Select",  se);
	KeyRow(col2,         row,      "Turbo A", ta);
	KeyRow(col2,         row + 16, "Turbo B", tb);
	KeyRow(col2,         row + 32, "Menu",    mn);
	KeyRow(col2,         row + 48, "D-pad",   Tr("Move"));
}

static void DrawPage(int page, int sel, int scroll, const char *status,
                     const char *prompt)
{
	const PAGE *pg = &pages[page];
	int i, shown;
	char value[64];

	//The paused game, dimmed, so the menu feels layered over it. OverlayDraw for the
	//same reason as in the wizard: the Video page picks the shell and the grid, and
	//cycling them behind a panel that hides them is no way to choose.
	VideoGuUpdate_Core(menuConfig.video.render, 1);
	OverlayDraw();
	oslSetAlpha(OSL_FX_ALPHA, 175);
	MyDrawFillRect(0, 0, 479, 271, GB_DARKEST);
	oslSetAlpha(OSL_FX_DEFAULT, 0);

	oslSetAlpha(OSL_FX_ALPHA, 240);
	MyDrawFillRect(PANEL_X, PANEL_TOP, PANEL_X + PANEL_W, PANEL_BOT, GB_DARKEST);
	oslSetAlpha(OSL_FX_DEFAULT, 0);
	DrawFrame(PANEL_X, PANEL_TOP, PANEL_X + PANEL_W, PANEL_BOT);

	oslSetFont(ftStandard);
	//The emulator leaves a translucent black text background set; the menu draws
	//its own panel, so text should sit directly on it.
	oslSetBkColor(RGBA(0, 0, 0, 0));

	oslSetTextColor(GB_LIGHT);
	oslDrawString(PANEL_X + 18, TITLE_Y, (char*)Tr(pg->title));
	MyDrawFillRect(PANEL_X + 18, TITLE_RULE, PANEL_X + PANEL_W - 18, TITLE_RULE + 1,
	               GB_DARK);

	if (page == P_LAYOUT)		{
		const char *pn = presetNames[(presetIndex >= 0 && presetIndex <= PRESET_COUNT)
		                             ? presetIndex : 0];
		DrawLayout();
		//Right-aligned on the title line: the key list below fills every row of the
		//panel, so there is no space left for a legend under the diagram.
		oslSetTextColor(GB_DARK);
		oslDrawString(PANEL_X + PANEL_W - 18 - GetStringWidth((char*)pn), TITLE_Y,
		              (char*)pn);
		oslDrawString(PANEL_X + 20, FOOT_HINT, (char*)Tr("O back"));
		return;
	}

	shown = pg->count - scroll;
	if (shown > VISIBLE_ROWS)
		shown = VISIBLE_ROWS;

	for (i = 0; i < shown; i++)		{
		const ITEM *it = &pg->items[scroll + i];
		int ry = ROW_TOP + i * ROW_H;
		int selected = (scroll + i == sel);

		if (selected)		{
			oslSetAlpha(OSL_FX_ALPHA, 90);
			MyDrawFillRect(PANEL_X + FRAME_T, ry - 3,
			               PANEL_X + PANEL_W - FRAME_T, ry + 13, GB_DARK);
			oslSetAlpha(OSL_FX_DEFAULT, 0);
			if (imgCursor)
				oslDrawImageXY(imgCursor, PANEL_X + 20, ry - 4);
			oslSetTextColor(GB_LIGHTEST);
		}
		else
			oslSetTextColor(GB_LIGHT);

		oslDrawString(PANEL_X + 40, ry, (char*)Tr(it->label));

		ItemValue(it, value, sizeof(value));
		if (value[0])		{
			oslSetTextColor(selected ? GB_LIGHTEST : GB_DARK);
			oslDrawString(PANEL_X + PANEL_W - 26 - GetStringWidth(value), ry, value);
		}
	}

	//Scroll markers, so it is obvious the list continues
	oslSetTextColor(GB_DARK);
	if (scroll > 0)
		oslDrawString(PANEL_X + PANEL_W - 28, ROW_TOP - 13, "^");
	if (scroll + VISIBLE_ROWS < pg->count)
		oslDrawString(PANEL_X + PANEL_W - 28, ROW_TOP + VISIBLE_ROWS * ROW_H - 6, "v");

	//Footer: what the highlighted row does, then the controls
	MyDrawFillRect(PANEL_X + 18, FOOT_RULE, PANEL_X + PANEL_W - 18, FOOT_RULE + 1,
	               GB_DARK);
	if (prompt)		{
		oslSetTextColor(GB_LIGHTEST);
		oslDrawString(PANEL_X + 20, FOOT_DESC, (char*)prompt);
	}
	else if (status && status[0])		{
		oslSetTextColor(GB_LIGHTEST);
		oslDrawString(PANEL_X + 20, FOOT_DESC, (char*)status);
	}
	else if (sel >= 0 && sel < pg->count &&
	         pg->items[sel].field == &menuConfig.video.bezel)		{
		int bz = menuConfig.video.bezel;
		if (bz > 0 && menuConfig.video.render != 2)		{
			oslSetTextColor(GB_LIGHTEST);
			oslDrawString(PANEL_X + 20, FOOT_DESC,
			              (char*)Tr("Only lines up with Screen size: Fit"));
		}
		else if (bz >= 0 && bz < 4)		{
			oslSetTextColor(GB_LIGHT);
			oslDrawString(PANEL_X + 20, FOOT_DESC, (char*)Tr(bezelDescs[bz]));
		}
	}
	else if (sel >= 0 && sel < pg->count &&
	         pg->items[sel].field == &menuConfig.video.overlay)		{
		//The overlays are drawn for the Fit viewport, so they only line up in Fit.
		//Say so rather than forcing the scaling - it is the player's choice.
		int ov = menuConfig.video.overlay;
		if (ov > 0 && menuConfig.video.render != 2)		{
			oslSetTextColor(GB_LIGHTEST);
			oslDrawString(PANEL_X + 20, FOOT_DESC,
			              (char*)Tr("Only lines up with Screen size: Fit"));
		}
		else if (ov >= 0 && ov < 6)		{
			oslSetTextColor(GB_LIGHT);
			oslDrawString(PANEL_X + 20, FOOT_DESC, (char*)Tr(filterDescs[ov]));
		}
	}
	else if (sel >= 0 && sel < pg->count && pg->items[sel].kind == K_PRESET)		{
		oslSetTextColor(GB_LIGHT);
		oslDrawString(PANEL_X + 20, FOOT_DESC,
		              (char*)Tr(presetDescs[(presetIndex >= 0 && presetIndex <= PRESET_COUNT)
		                                    ? presetIndex : 0]));
	}
	else if (sel >= 0 && sel < pg->count && pg->items[sel].desc)		{
		oslSetTextColor(GB_LIGHT);
		oslDrawString(PANEL_X + 20, FOOT_DESC, (char*)Tr(pg->items[sel].desc));
	}

	oslSetTextColor(GB_DARK);
	oslDrawString(PANEL_X + 20, FOOT_HINT, (char*)Tr("X select  O back  [] default"));
}

//--- actions -----------------------------------------------------------------

static int DoAction(int action, char *status, int size, int sel, int scroll)
{
	status[0] = '\0';
	switch (action)		{
		case A_RESUME:
			return 1;
		case A_SAVESTATE:
			snprintf(status, size, pspSaveState(stateSlot)
			         ? Tr("Saved to slot %i") : Tr("Could not save slot %i"), stateSlot);
			break;
		case A_LOADSTATE:
			snprintf(status, size, pspLoadState(stateSlot)
			         ? Tr("Loaded slot %i") : Tr("No state in slot %i"), stateSlot);
			break;
		case A_SAVENOW:
			//force = 1: write even if the CRC says nothing changed
			machine_manage_sram(SRAM_SAVE, 1);
			strncpy(status, Tr("Game saved"), size - 1);
			break;
		case A_DEFAULTS_ALL:		{
			//Settings go back; the loaded ROM stays loaded
			char rom[MAX_PATH];
			strcpy(rom, menuConfig.file.filename);
			memcpy(&menuConfig, menuConfigUserDefault, sizeof(MENUPARAMS));
			strcpy(menuConfig.file.filename, rom);
			ScalingPull();
			PresetPull();
			SaveUserDefaultConfig();
			strncpy(status, Tr("All settings reset"), size - 1);
			break;
		}
		case A_DELETE_AUTO:		{
			char statePath[MAX_PATH];
			pspGetStateNameEx(menuConfig.file.filename, statePath, STATE_AUTO);
			if (sceIoRemove(statePath) >= 0)
				strncpy(status, Tr("Resume state deleted"), size - 1);
			else
				strncpy(status, Tr("No resume state"), size - 1);
			break;
		}
		case A_BACK:
			//handled by the caller, which owns the page stack
			break;
		case A_RESET:
			machine_reset();
			return 1;
		case A_SLEEP:		{
			//Flush game battery save and configuration before suspending: sleep is
			//not guaranteed to be woken from, and a flat battery should not cost
			//progress.
			int spin = 0;
			machine_manage_sram(SRAM_SAVE, 1);
			SaveUserDefaultConfig();
			//Suspend with Cross still held and the press is waiting for the game on
			//the other side of the wake. Let go first, with a ceiling so a stuck or
			//drifting pad cannot keep us awake forever.
			while (!osl_quit && spin < 180 &&
			       (osl_keys->held.value & STDKEYMASK))		{
				MyReadKeys();
				spin++;
				oslStartDrawing();
				DrawPage(P_MAIN, sel, scroll, Tr("Release the button to sleep"), NULL);
				oslEndDrawing();
				oslSyncFrame();
			}
			scePowerTick(0);
			scePowerRequestSuspend();
			osl_keys->pressed.value = 0;
			//The request is asynchronous, so a few frames of the game still run
			//before the machine actually goes down, and the wake lands back here.
			//Hold input off across both.
			gblSwallowInput = 120;
			return 1;
		}
		case A_QUIT:
			//SmsTerm() writes the auto state and then the battery save on the way
			//out, so just ask the loop to finish rather than doing it here.
			osl_quit = 1;
			return 1;
	}
	status[size - 1] = '\0';
	return 0;
}

//--- main loop ---------------------------------------------------------------

//Asked at boot when "Resume on launch" is set to Ask. Drawn in the menu's own style
//rather than through MasterBoy's window system, and deliberately defaults to Yes.
int GameMenuAskResume(void)
{
	int yes = 1, done = 0, result = 1;

	LoadCursor();
	SfxInit();
	MenuStringsInit(menuConfig.file.filename);
	oslSetFramerate(60);
	osl_keys->pressed.value = 0;

	while (!osl_quit && !done)		{
		int bx = 128, by = 96, bw = 224, bh = 88;

		MyReadKeys();

		if (osl_keys->pressed.left || osl_keys->pressed.right ||
		    osl_keys->pressed.up || osl_keys->pressed.down)		{
			yes = !yes;
			SfxPlay(SFX_MOVE);
		}
		if (osl_keys->pressed.cross)		{
			result = yes;
			done = 1;
			SfxPlay(SFX_SELECT);
		}
		if (osl_keys->pressed.circle)		{
			result = 0;
			done = 1;
			SfxPlay(SFX_BACK);
		}

		oslStartDrawing();
		oslSetAlpha(OSL_FX_ALPHA, 200);
		MyDrawFillRect(0, 0, 479, 271, GB_DARKEST);
		oslSetAlpha(OSL_FX_DEFAULT, 0);

		MyDrawFillRect(bx, by, bx + bw, by + bh, GB_DARKEST);
		MyDrawFillRect(bx, by, bx + 3, by + bh, GB_LIGHT);
		MyDrawFillRect(bx, by, bx + bw, by + 1, GB_DARK);
		MyDrawFillRect(bx, by + bh - 1, bx + bw, by + bh, GB_DARK);

		oslSetFont(ftStandard);
		oslSetBkColor(RGBA(0, 0, 0, 0));
		oslSetTextColor(GB_LIGHTEST);
		oslDrawString(bx + 18, by + 16, (char*)Tr("Continue where you left off?"));

		oslSetTextColor(yes ? GB_LIGHTEST : GB_DARK);
		oslDrawString(bx + 42, by + 52, (char*)Tr("Resume"));
		oslSetTextColor(yes ? GB_DARK : GB_LIGHTEST);
		oslDrawString(bx + 138, by + 52, (char*)Tr("New"));
		if (imgCursor)
			oslDrawImageXY(imgCursor, (yes ? bx + 22 : bx + 118) - 4, by + 48);

		oslEndDrawing();
		oslSyncFrame();
	}

	osl_keys->pressed.value = 0;
	//The dismissing press is still held; keep it out of the game for up to two
	//seconds, or until the pad is released
	gblSwallowInput = 120;
	return result;
}

//Prompt the user to restart now or later when machine type or colourise changes
static int GameMenuAskRestart(const char *reason)
{
	int yes = 1, done = 0, result = 1;

	LoadCursor();
	SfxInit();
	MenuStringsInit(menuConfig.file.filename);
	oslSetFramerate(60);
	osl_keys->pressed.value = 0;

	while (!osl_quit && !done)		{
		int bx = 112, by = 90, bw = 256, bh = 92;

		MyReadKeys();

		if (osl_keys->pressed.left || osl_keys->pressed.right ||
		    osl_keys->pressed.up || osl_keys->pressed.down)		{
			yes = !yes;
			SfxPlay(SFX_MOVE);
		}
		if (osl_keys->pressed.cross)		{
			result = yes;
			done = 1;
			SfxPlay(SFX_SELECT);
		}
		if (osl_keys->pressed.circle)		{
			result = 0;
			done = 1;
			SfxPlay(SFX_BACK);
		}

		oslStartDrawing();
		oslSetAlpha(OSL_FX_ALPHA, 200);
		MyDrawFillRect(0, 0, 479, 271, GB_DARKEST);
		oslSetAlpha(OSL_FX_DEFAULT, 0);

		MyDrawFillRect(bx, by, bx + bw, by + bh, GB_DARKEST);
		MyDrawFillRect(bx, by, bx + 3, by + bh, GB_LIGHT);
		MyDrawFillRect(bx, by, bx + bw, by + 1, GB_DARK);
		MyDrawFillRect(bx, by + bh - 1, bx + bw, by + bh, GB_DARK);

		oslSetFont(ftStandard);
		oslSetBkColor(RGBA(0, 0, 0, 0));
		oslSetTextColor(GB_LIGHTEST);
		oslDrawString(bx + 18, by + 14, (char*)Tr(reason ? reason : "Hardware changed."));
		oslDrawString(bx + 18, by + 30, (char*)Tr("Restart game now to apply?"));

		oslSetTextColor(yes ? GB_LIGHTEST : GB_DARK);
		oslDrawString(bx + 48, by + 60, (char*)Tr("Restart"));
		oslSetTextColor(yes ? GB_DARK : GB_LIGHTEST);
		oslDrawString(bx + 158, by + 60, (char*)Tr("Later"));
		if (imgCursor)
			oslDrawImageXY(imgCursor, (yes ? bx + 28 : bx + 138) - 4, by + 56);

		oslEndDrawing();
		oslSyncFrame();
	}

	osl_keys->pressed.value = 0;
	gblSwallowInput = 120;
	return result;
}

//Shown once, after the ROM is loaded and any snapshot restored, before the first
//frame of play. Two jobs: say whether this is a continue or a fresh start, and let
//the player press a button when they are ready rather than being dropped straight
//into the middle of a level.
void GameMenuStartGate(int resumed)
{
	int done = 0, frame = 0;

	LoadCursor();
	SfxInit();
	MenuStringsInit(menuConfig.file.filename);
	oslSetFramerate(60);
	osl_keys->pressed.value = 0;

	//Let go of whatever launched us first
	while (!osl_quit && (osl_keys->held.value & 0xf80f3f9))
		MyReadKeys();

	while (!osl_quit && !done)		{
		int bx = 96, by = 92, bw = 288, bh = 92;
		const char *line = resumed ? Tr("Continuing where you left off")
		                           : Tr("Ready to play");

		MyReadKeys();
		frame++;
		if (osl_keys->pressed.value & 0xf80f3f9)		{
			done = 1;
			SfxPlay(SFX_SELECT);
		}

		oslStartDrawing();
		//Whatever the emulator has drawn so far, dimmed right down
		VideoGuUpdate_Core(menuConfig.video.render, 1);
		oslSetAlpha(OSL_FX_ALPHA, 200);
		MyDrawFillRect(0, 0, 479, 271, GB_DARKEST);
		oslSetAlpha(OSL_FX_DEFAULT, 0);

		MyDrawFillRect(bx, by, bx + bw, by + bh, GB_DARKEST);
		MyDrawFillRect(bx, by, bx + 3, by + bh, GB_LIGHT);
		MyDrawFillRect(bx, by, bx + bw, by + 1, GB_DARK);
		MyDrawFillRect(bx, by + bh - 1, bx + bw, by + bh, GB_DARK);

		oslSetFont(ftStandard);
		oslSetBkColor(RGBA(0, 0, 0, 0));

		oslSetTextColor(GB_LIGHT);
		oslDrawString(bx + 20, by + 16, (char*)Tr("RISE OF THE PENGUINS GB"));
		MyDrawFillRect(bx + 20, by + 32, bx + bw - 20, by + 33, GB_DARK);

		oslSetTextColor(GB_LIGHTEST);
		oslDrawString(bx + 20, by + 42, (char*)line);

		//Blink, so it reads as waiting for you rather than frozen
		if ((frame / 30) & 1)
			oslSetTextColor(GB_LIGHT);
		else
			oslSetTextColor(GB_DARK);
		oslDrawString(bx + 20, by + 66, (char*)Tr("Press any button to continue"));

		if (imgCursor)
			oslDrawImageXY(imgCursor, bx + bw - 34, by + 58);

		oslEndDrawing();
		oslSyncFrame();
	}

	osl_keys->pressed.value = 0;
	//The dismissing press is still held; keep it out of the game for up to two
	//seconds, or until the pad is released
	gblSwallowInput = 120;
}

//--- first run ---------------------------------------------------------------
//The launcher opens straight into the game, which gives no hint that any of it can
//be changed, so the handful of choices that actually change how it feels get asked
//once, up front. Everything here is also on the normal menu pages; this is about
//discoverability, not about a setting that can only be reached here. The wording
//stays on the game rather than on the emulator underneath it - a player who bought
//Rise of the Penguins GB did not ask to be told what a Game Boy is.
//
//"Done" is a marker file rather than a config key: DEFAULT.INI ships with the
//launcher, so its presence says nothing about whether anyone has ever played.

static void OnboardPath(char *dst, int size)
{
	if (gblAppPath[0])
		snprintf(dst, size, "%s/setup.done", gblAppPath);
	else
		safe_strcpy(dst, "setup.done", size);
}

static int OnboardDone(void)
{
	char path[MAX_PATH];
	SceUID fd;
	OnboardPath(path, sizeof(path));
	fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
	if (fd < 0)
		return 0;
	sceIoClose(fd);
	return 1;
}

static void OnboardMarkDone(void)
{
	char path[MAX_PATH];
	SceUID fd;
	OnboardPath(path, sizeof(path));
	fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd >= 0)		{
		sceIoWrite(fd, "1", 1);
		sceIoClose(fd);
	}
}

typedef struct {
	const char *title;
	const char *line1;
	const char *line2;
	int *field;			//NULL on a step that only says something
	const char **names;
	int count;
	int preset;			//edits the layout preset instead of a plain field
} STEP;

static const STEP onboardSteps[] = {
	{"RISE OF THE PENGUINS GB", "Welcome. A few quick choices", "before you set off.",
	 0, 0, 0, 0},
	{"BUTTONS", "Which way round do you want", "A and B?",
	 0, 0, 0, 1},
	{"SCREEN", "Frame the game in a handheld", "shell?",
	 &menuConfig.video.bezel, bezelNames, 4, 0},
	{"SCREEN", "Lay an LCD texture over it?", 0,
	 &menuConfig.video.overlay, filterNames, 6, 0},
	{"SOUND", "Music and sound effects?", 0,
	 &menuConfig.sound.enabled, onOff, 2, 0},
	{"READY", "Hold L while playing to open the", "menu and change any of this.",
	 0, 0, 0, 0},
};

//Runs once, before the first frame of play. Returns 1 if it ran.
int GameMenuOnboard(void)
{
	int step = 0, done = 0, frame = 0;
	int nsteps = numberof(onboardSteps);

	if (OnboardDone())
		return 0;

	LoadCursor();
	SfxInit();
	MenuStringsInit(menuConfig.file.filename);
	PresetPull();
	if (!presetIndex)
		presetIndex = 1;
	oslSetFramerate(60);
	oslSetKeyAutorepeatInit(24);
	oslSetKeyAutorepeatInterval(6);
	osl_keys->autoRepeatMask = OSL_KEYMASK_LEFT | OSL_KEYMASK_RIGHT;
	osl_keys->pressed.value = 0;

	//Let go of whatever launched us, or the first step is skipped instantly
	while (!osl_quit && (osl_keys->held.value & STDKEYMASK))
		MyReadKeys();

	while (!osl_quit && !done)		{
		const STEP *st = &onboardSteps[step];
		//Outer box, then the content rect inside the frame band. Everything below
		//positions off the content rect, so the art can change thickness without
		//every offset here having to be found and adjusted.
		int ox = 64, oy = 44, ow = 352, oh = 184;
		int bx = ox + FRAME_T, by = oy + FRAME_T;
		int bw = ow - 2 * FRAME_T, bh = oh - 2 * FRAME_T;
		char value[64];

		MyReadKeys();
		frame++;

		if (st->preset)		{
			if (osl_keys->pressed.left)		{
				PresetApply(presetIndex > 1 ? presetIndex - 1 : PRESET_COUNT);
				SfxPlay(SFX_MOVE);
			}
			if (osl_keys->pressed.right)		{
				PresetApply(presetIndex < PRESET_COUNT ? presetIndex + 1 : 1);
				SfxPlay(SFX_MOVE);
			}
		}
		else if (st->field)		{
			int dir = osl_keys->pressed.right ? 1 : (osl_keys->pressed.left ? -1 : 0);
			if (dir)		{
				*st->field += dir;
				if (*st->field < 0)
					*st->field = st->count - 1;
				else if (*st->field >= st->count)
					*st->field = 0;
				SfxPlay(SFX_MOVE);
			}
		}

		if (osl_keys->pressed.cross || osl_keys->pressed.start)		{
			SfxPlay(SFX_SELECT);
			if (++step >= nsteps)
				done = 1;
		}
		if (osl_keys->pressed.circle)		{
			SfxPlay(SFX_BACK);
			if (step > 0)
				step--;
		}
		if (step < 0)
			step = 0;
		if (step >= nsteps)
			step = nsteps - 1;
		st = &onboardSteps[step];

		//Before oslStartDrawing: the shell and grid images are loaded here, so the
		//choice on those two steps is previewed live behind the panel.
		OverlaySync();

		oslStartDrawing();
		VideoGuUpdate_Core(menuConfig.video.render, 1);
		//VideoGuUpdate_Core draws the picture; the shell and grid come from
		//OverlayDraw, which normally runs a level up in VideoGuUpdate. Without it
		//those two steps would be choosing something you cannot see.
		OverlayDraw();
		oslSetAlpha(OSL_FX_ALPHA, 190);
		MyDrawFillRect(0, 0, 479, 271, GB_DARKEST);
		oslSetAlpha(OSL_FX_DEFAULT, 0);

		MyDrawFillRect(ox, oy, ox + ow, oy + oh, GB_DARKEST);
		DrawFrame(ox, oy, ox + ow, oy + oh);

		oslSetFont(ftStandard);
		oslSetBkColor(RGBA(0, 0, 0, 0));

		oslSetTextColor(GB_LIGHT);
		oslDrawString(bx + 20, by + 14, (char*)Tr(st->title));
		//Which of the steps this is, so the end is in sight
		{
			char pos[16];
			snprintf(pos, sizeof(pos), "%i/%i", step + 1, nsteps);
			oslDrawString(bx + bw - 20 - GetStringWidth(pos), by + 14, pos);
		}
		MyDrawFillRect(bx + 20, by + 30, bx + bw - 20, by + 31, GB_DARK);

		oslSetTextColor(GB_LIGHTEST);
		if (st->line1)
			oslDrawString(bx + 20, by + 42, (char*)Tr(st->line1));
		if (st->line2)
			oslDrawString(bx + 20, by + 58, (char*)Tr(st->line2));

		if (st->preset || st->field)		{
			int vy = by + 88;
			if (st->preset)
				safe_strcpy(value, Tr(presetNames[presetIndex]), sizeof(value));
			else
				safe_strcpy(value, Tr(st->names[*st->field]), sizeof(value));

			//Arrows either side, so it reads as something to change rather than as
			//a label
			oslSetTextColor(GB_LIGHT);
			oslDrawString(bx + 24, vy, "<");
			oslDrawString(bx + bw - 32, vy, ">");
			oslSetTextColor(GB_LIGHTEST);
			oslDrawString(bx + bw / 2 - GetStringWidth(value) / 2, vy, value);

			oslSetTextColor(GB_LIGHT);
			if (st->preset)
				oslDrawString(bx + 20, vy + 20, (char*)Tr(presetDescs[presetIndex]));
			else if (st->field == &menuConfig.video.overlay)
				oslDrawString(bx + 20, vy + 20, (char*)Tr(filterDescs[*st->field]));
			else if (st->field == &menuConfig.video.bezel)
				oslDrawString(bx + 20, vy + 20, (char*)Tr(bezelDescs[*st->field]));
		}
		else if ((frame / 30) & 1)		{
			oslSetTextColor(GB_LIGHT);
			oslDrawString(bx + 20, by + 96, (char*)Tr("Press X to continue"));
		}

		oslSetTextColor(GB_DARK);
		if (!step)
			oslDrawString(bx + 20, by + bh - 22, (char*)Tr("X start"));
		else if (step == nsteps - 1)
			oslDrawString(bx + 20, by + bh - 22, (char*)Tr("X play  O back"));
		else if (st->preset || st->field)
			oslDrawString(bx + 20, by + bh - 22, (char*)Tr("X next  O back  < > change"));
		else
			oslDrawString(bx + 20, by + bh - 22, (char*)Tr("X next  O back"));

		if (imgCursor)
			oslDrawImageXY(imgCursor, bx + bw - 34, by + bh - 26);

		oslEndDrawing();
		oslSyncFrame();
	}

	//Keep what was chosen, and do not ask again
	OverlaySync();
	SaveUserDefaultConfig();
	OnboardMarkDone();

	osl_keys->pressed.value = 0;
	gblSwallowInput = 120;
	return 1;
}

//Where the menu was when it last closed. Kept between openings so that changing a
//setting, closing to look at it and opening again lands back on the same row
//instead of at the top of the main page every time.
static int savedPage = P_MAIN, savedSel = 0, savedScroll = 0, savedDepth = 0;
static int savedStackPage[4], savedStackSel[4], savedStackScroll[4];

void GameMenuShow(void)
{
	int page, sel, scroll, quit = 0, statusTime = 0;
	int initialGbType = menuConfig.gameboy.gbType;
	int initialColorization = menuConfig.gameboy.colorization;
	//L is still held from opening the menu; ignore it until released, or the menu
	//closes on the same press that opened it.
	int menuKeyHeld = 1;
	int stackPage[4], stackSel[4], stackScroll[4], depth;
	char status[64];
	int i;

	page = savedPage;
	sel = savedSel;
	scroll = savedScroll;
	depth = savedDepth;
	for (i = 0; i < 4; i++)		{
		stackPage[i] = savedStackPage[i];
		stackSel[i] = savedStackSel[i];
		stackScroll[i] = savedStackScroll[i];
	}

	//Be forgiving about anything stale
	if (page < 0 || page >= P_COUNT)
		page = P_MAIN;
	if (depth < 0 || depth > 4)
		depth = 0;
	if (sel < 0 || sel >= pages[page].count)
		sel = 0;

	status[0] = '\0';
	LoadCursor();
	SfxInit();
	MenuStringsInit(menuConfig.file.filename);
	ScalingPull();
	PaletteScan();
	PalettePull();
	PresetPull();

	oslSetFramerate(60);
	oslSetKeyAutorepeatInit(24);
	oslSetKeyAutorepeatInterval(6);
	//Only the d-pad repeats. Letting Cross/Circle repeat meant a slightly long press
	//walked several rows or popped straight out of the menu.
	osl_keys->autoRepeatMask = OSL_KEYMASK_UP | OSL_KEYMASK_DOWN |
	                           OSL_KEYMASK_LEFT | OSL_KEYMASK_RIGHT;
	osl_keys->pressed.value = 0;

	while (!osl_quit && !quit)		{
		const PAGE *pg = &pages[page];
		const ITEM *it = &pg->items[sel];

		MyReadKeys();

		if (osl_keys->pressed.up)		{
			sel = (sel + pg->count - 1) % pg->count;
			status[0] = '\0';
			SfxPlay(SFX_MOVE);
		}
		if (osl_keys->pressed.down)		{
			sel = (sel + 1) % pg->count;
			status[0] = '\0';
			SfxPlay(SFX_MOVE);
		}
		if (osl_keys->pressed.left)		{
			ItemAdjust(it, -1);
			ScalingPush();
			status[0] = '\0';
			SfxPlay(SFX_MOVE);
		}
		if (osl_keys->pressed.right)		{
			ItemAdjust(it, 1);
			ScalingPush();
			status[0] = '\0';
			SfxPlay(SFX_MOVE);
		}

		//Square reverts just this setting
		if (osl_keys->pressed.square)		{
			ResetItem(it);
			ScalingPush();
			PresetPull();
			safe_strcpy(status, Tr("Reverted to default"), sizeof(status));
			statusTime = 120;
			SfxPlay(SFX_SELECT);
		}

		if (osl_keys->pressed.cross)		{
			SfxPlay(SFX_SELECT);
			if (it->kind == K_LINK && depth < 4)		{
				stackPage[depth] = page;
				stackSel[depth] = sel;
				stackScroll[depth] = scroll;
				depth++;
				page = it->arg;
				sel = 0;
				scroll = 0;
				status[0] = '\0';
			}
			else if (it->kind == K_ACTION && it->arg == A_BACK)		{
				if (depth > 0)		{
					depth--;
					page = stackPage[depth];
					sel = stackSel[depth];
					scroll = stackScroll[depth];
					status[0] = '\0';
				}
				else
					quit = 1;
			}
			else if (it->kind == K_ACTION)		{
				quit = DoAction(it->arg, status, sizeof(status), sel, scroll);
				statusTime = 120;
			}
			else if (it->kind == K_KEY)		{
				u32 k = CaptureKey(page, sel, scroll, it->label);
				if (k)
					menuConfig.ctrl.akeys[it->arg] = k;
				//A hand edit may have moved off a preset, or back onto one
				PresetPull();
			}
			else if (it->kind == K_CUT)		{
				u32 k = CaptureKey(page, sel, scroll, it->label);
				if (k)
					menuConfig.ctrl.acuts[it->arg] = k;
			}
			else		{
				ItemAdjust(it, 1);
				ScalingPush();
			}
		}

		if (osl_keys->pressed.circle)		{
			SfxPlay(SFX_BACK);
			if (depth > 0)		{
				depth--;
				page = stackPage[depth];
				sel = stackSel[depth];
				scroll = stackScroll[depth];
				status[0] = '\0';
			}
			else
				quit = 1;
		}

		//L again closes it, matching how it was opened
		if (menuGetMenuKey((u32*)&osl_keys->held.value, MENUKEY_MENU))		{
			if (!menuKeyHeld)
				quit = 1;
		}
		else
			menuKeyHeld = 0;

		//Keep the selection inside the visible window
		pg = &pages[page];
		if (sel < scroll)
			scroll = sel;
		else if (sel >= scroll + VISIBLE_ROWS)
			scroll = sel - VISIBLE_ROWS + 1;
		if (scroll > pg->count - VISIBLE_ROWS)
			scroll = pg->count - VISIBLE_ROWS;
		if (scroll < 0)
			scroll = 0;

		if (statusTime > 0 && --statusTime == 0)
			status[0] = '\0';

		oslStartDrawing();
		DrawPage(page, sel, scroll, status, NULL);
		oslEndDrawing();
		oslSyncFrame();
	}

	ScalingPush();

	//Remember where we were for next time
	savedPage = page;
	savedSel = sel;
	savedScroll = scroll;
	savedDepth = depth;
	for (i = 0; i < 4; i++)		{
		savedStackPage[i] = stackPage[i];
		savedStackSel[i] = stackSel[i];
		savedStackScroll[i] = stackScroll[i];
	}

	//The filter may have changed; pick it up before the game redraws
	OverlaySync();
	//Save any settings adjusted in the menu so they survive relaunch
	SaveUserDefaultConfig();

	//If the machine type or colorization setting changed, prompt to restart to take effect
	if (menuConfig.gameboy.gbType != initialGbType ||
	    menuConfig.gameboy.colorization != initialColorization) {
		const char *reason = (menuConfig.gameboy.gbType != initialGbType)
		                     ? "Machine type changed."
		                     : "Colourise mode changed.";
		if (GameMenuAskRestart(reason)) {
			machine_reset();
		}
	}

	osl_keys->pressed.value = 0;
	//The dismissing press is still held; keep it out of the game for up to two
	//seconds, or until the pad is released
	gblSwallowInput = 120;
}
