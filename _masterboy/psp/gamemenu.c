//=== IN-GAME MENU ===
//See gamemenu.h. Drawn in the game's own four-colour Game Boy palette, as a
//vertical list, so that pressing L reads as part of the game rather than as
//MasterBoy's carousel.

#include <pspaudio.h>
#include "pspcommon.h"
#include "gamemenu.h"

//The DMG palette the game's own art uses, lightest to darkest.
#define GB_LIGHTEST	RGB(224, 248, 208)
#define GB_LIGHT	RGB(136, 192, 112)
#define GB_DARK		RGB(52, 104, 86)
#define GB_DARKEST	RGB(8, 24, 32)

#define PANEL_X		72
#define PANEL_W		336
#define PANEL_TOP	18
#define PANEL_BOT	258
#define ROW_H		19
#define ROW_TOP		54
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

static OSL_IMAGE *imgCursor = NULL;
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
	int now = (int)osl_frameRateCounter;
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
	K_PALETTE	//cycles the palettes.ini list
};

enum {
	P_MAIN = 0, P_VIDEO, P_AUDIO, P_CONTROLS, P_BUTTONS, P_SHORTCUTS, P_SAVE,
	P_LAYOUT, P_GAMEBOY, P_COUNT
};

enum {
	A_RESUME = 1, A_SAVESTATE, A_LOADSTATE, A_SAVENOW, A_RESET, A_QUIT,
	A_DEFAULTS_ALL, A_DELETE_AUTO, A_BACK
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
static const char *gbTypeNames[] = {"Auto", "Game Boy", "Super GB", "GB Color", "GB Advance"};

//menuConfig.video.render is not a 0..n index (see menuMainVideoScalingItems in
//menuplus.c), so the menu edits a shadow index and maps it back.
static int scalingIndexShadow;

static const ITEM pageMain[] = {
	{"Resume",             K_ACTION, 0, 0, 0, 0, 0, A_RESUME},
	{"Video",              K_LINK,   0, 0, 0, 0, 0, P_VIDEO},
	{"Audio",              K_LINK,   0, 0, 0, 0, 0, P_AUDIO},
	{"Controls",           K_LINK,   0, 0, 0, 0, 0, P_CONTROLS},
	{"Save / Load",        K_LINK,   0, 0, 0, 0, 0, P_SAVE},
	{"Reset all settings", K_ACTION, 0, 0, 0, 0, 0, A_DEFAULTS_ALL,
	 "Put every option back to how this launcher ships"},
	{"Restart game",       K_ACTION, 0, 0, 0, 0, 0, A_RESET},
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
	{"Button layout",   K_LINK,  0, 0, 0, 0, 0, P_LAYOUT,
	 "See what every button currently does"},
	{"Buttons",         K_LINK,  0, 0, 0, 0, 0, P_BUTTONS},
	{"Shortcuts",       K_LINK,  0, 0, 0, 0, 0, P_SHORTCUTS},
	{"Analog as D-pad", K_ENUM,  &menuConfig.ctrl.analog.toPad,    onOff, 2, 0, 0, 0},
	{"Analog deadzone", K_RANGE, &menuConfig.ctrl.analog.treshold, 0, 1, 127, 4, 0,
	 "How far the stick must move before it counts"},
	{"Autofire speed",  K_RANGE, &menuConfig.ctrl.autofireRate,    0, 1, 10, 1, 0},
};

//Order matches the akeys union in menuplus.h
static const ITEM pageButtons[] = {
	{"Up",      K_KEY, 0, 0, 0, 0, 0, 0},
	{"Down",    K_KEY, 0, 0, 0, 0, 0, 1},
	{"Left",    K_KEY, 0, 0, 0, 0, 0, 2},
	{"Right",   K_KEY, 0, 0, 0, 0, 0, 3},
	{"A",       K_KEY, 0, 0, 0, 0, 0, 4},
	{"B",       K_KEY, 0, 0, 0, 0, 0, 5},
	{"Start",   K_KEY, 0, 0, 0, 0, 0, 6},
	{"Turbo A", K_KEY, 0, 0, 0, 0, 0, 7},
	{"Turbo B", K_KEY, 0, 0, 0, 0, 0, 8},
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
	{"Machine type", K_ENUM, &menuConfig.gameboy.gbType, gbTypeNames, 5, 0, 0, 0,
	 "Restart the game for a change here to take effect"},
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
				strncpy(dst, rateNames[RateIndex()], size - 1);
			else if (*it->field >= 0 && *it->field < it->lo)
				strncpy(dst, it->names[*it->field], size - 1);
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
				strncpy(dst, "Game's own", size - 1);
			else
				strncpy(dst, paletteNames[paletteIndex - 1], size - 1);
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
	}
}

//--- key capture -------------------------------------------------------------
//MasterBoy's own redefinition routine is tied to its window system, so the menu
//captures keys itself and draws the prompt in its own panel.
#define STDKEYMASK 0xf80f3f9

static void DrawPage(int page, int sel, int scroll, const char *status,
                     const char *prompt);

static u32 CaptureKey(int page, int sel, int scroll, const char *label)
{
	char prompt[80];
	u32 value = 0;
	int settled = 0, waited = 0;

	snprintf(prompt, sizeof(prompt), "Press a button for %s", label);

	//Let go of the button that opened this first
	while (!osl_quit && (osl_keys->held.value & STDKEYMASK))		{
		MyReadKeys();
		oslStartDrawing();
		DrawPage(page, sel, scroll, NULL, prompt);
		oslEndDrawing();
		oslSyncFrame();
	}

	//Then take the next combination, once held steady for a moment
	while (!osl_quit && settled < 20 && waited < 400)		{
		u32 now;
		MyReadKeys();
		now = osl_keys->held.value & STDKEYMASK;
		if (now && now == value)
			settled++;
		else		{
			value = now;
			settled = 0;
		}
		waited++;
		oslStartDrawing();
		DrawPage(page, sel, scroll, NULL, prompt);
		oslEndDrawing();
		oslSyncFrame();
	}

	//Wait for release, or the new binding fires immediately
	while (!osl_quit && (osl_keys->held.value & STDKEYMASK))		{
		MyReadKeys();
		oslStartDrawing();
		DrawPage(page, sel, scroll, NULL, prompt);
		oslEndDrawing();
		oslSyncFrame();
	}
	return value;
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
	oslDrawString(x, y, (char*)name);
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
	int col2 = PANEL_X + 176, row = 176;

	menuGetKeyName(a,  sizeof(a),  "/", menuConfig.ctrl.akeys[4]);
	menuGetKeyName(b,  sizeof(b),  "/", menuConfig.ctrl.akeys[5]);
	menuGetKeyName(ta, sizeof(ta), "/", menuConfig.ctrl.akeys[7]);
	menuGetKeyName(tb, sizeof(tb), "/", menuConfig.ctrl.akeys[8]);
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
	oslDrawString((cx + fx) / 2 - GetStringWidth("GAME") / 2, cy - 12, "GAME");

	//D-pad
	Cap(cx, cy - sp, cw, cw, "", 1);
	Cap(cx, cy + sp, cw, cw, "", 1);
	Cap(cx - sp, cy, cw, cw, "", 1);
	Cap(cx + sp, cy, cw, cw, "", 1);
	Cap(cx, cy, cw, cw, "", 0);

	//Face buttons, in their real positions
	Cap(fx, fy - sp, cw, cw, "T", menuConfig.ctrl.akeys[8] != 0);
	Cap(fx, fy + sp, cw, cw, "X", menuConfig.ctrl.akeys[4] != 0);
	Cap(fx - sp, fy, cw, cw, "S", menuConfig.ctrl.akeys[7] != 0);
	Cap(fx + sp, fy, cw, cw, "O", menuConfig.ctrl.akeys[5] != 0);

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
	KeyRow(col2,         row + 48, "D-pad",   "Move");
}

static void DrawPage(int page, int sel, int scroll, const char *status,
                     const char *prompt)
{
	const PAGE *pg = &pages[page];
	int i, shown;
	char value[64];

	//The paused game, dimmed, so the menu feels layered over it
	VideoGuUpdate_Core(menuConfig.video.render, 1);
	oslSetAlpha(OSL_FX_ALPHA, 175);
	MyDrawFillRect(0, 0, 479, 271, GB_DARKEST);
	oslSetAlpha(OSL_FX_DEFAULT, 0);

	oslSetAlpha(OSL_FX_ALPHA, 240);
	MyDrawFillRect(PANEL_X, PANEL_TOP, PANEL_X + PANEL_W, PANEL_BOT, GB_DARKEST);
	oslSetAlpha(OSL_FX_DEFAULT, 0);
	MyDrawFillRect(PANEL_X, PANEL_TOP, PANEL_X + 3, PANEL_BOT, GB_LIGHT);
	MyDrawFillRect(PANEL_X, PANEL_TOP, PANEL_X + PANEL_W, PANEL_TOP + 1, GB_DARK);
	MyDrawFillRect(PANEL_X, PANEL_BOT - 1, PANEL_X + PANEL_W, PANEL_BOT, GB_DARK);

	oslSetFont(ftStandard);
	//The emulator leaves a translucent black text background set; the menu draws
	//its own panel, so text should sit directly on it.
	oslSetBkColor(RGBA(0, 0, 0, 0));

	oslSetTextColor(GB_LIGHT);
	oslDrawString(PANEL_X + 18, 26, (char*)pg->title);
	MyDrawFillRect(PANEL_X + 18, 42, PANEL_X + PANEL_W - 18, 43, GB_DARK);

	if (page == P_LAYOUT)		{
		DrawLayout();
		oslSetTextColor(GB_DARK);
		oslDrawString(PANEL_X + 20, PANEL_BOT - 22, "O back");
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
			MyDrawFillRect(PANEL_X + 10, ry - 3, PANEL_X + PANEL_W - 10, ry + 13,
			               GB_DARK);
			oslSetAlpha(OSL_FX_DEFAULT, 0);
			if (imgCursor)
				oslDrawImageXY(imgCursor, PANEL_X - 6, ry - 4);
			oslSetTextColor(GB_LIGHTEST);
		}
		else
			oslSetTextColor(GB_LIGHT);

		oslDrawString(PANEL_X + 20, ry, (char*)it->label);

		ItemValue(it, value, sizeof(value));
		if (value[0])		{
			oslSetTextColor(selected ? GB_LIGHTEST : GB_DARK);
			oslDrawString(PANEL_X + PANEL_W - 20 - GetStringWidth(value), ry, value);
		}
	}

	//Scroll markers, so it is obvious the list continues
	oslSetTextColor(GB_DARK);
	if (scroll > 0)
		oslDrawString(PANEL_X + PANEL_W - 16, ROW_TOP - 13, "^");
	if (scroll + VISIBLE_ROWS < pg->count)
		oslDrawString(PANEL_X + PANEL_W - 16, ROW_TOP + VISIBLE_ROWS * ROW_H - 6, "v");

	//Footer: what the highlighted row does, then the controls
	MyDrawFillRect(PANEL_X + 18, PANEL_BOT - 36, PANEL_X + PANEL_W - 18,
	               PANEL_BOT - 35, GB_DARK);
	if (prompt)		{
		oslSetTextColor(GB_LIGHTEST);
		oslDrawString(PANEL_X + 20, PANEL_BOT - 28, (char*)prompt);
	}
	else if (status && status[0])		{
		oslSetTextColor(GB_LIGHTEST);
		oslDrawString(PANEL_X + 20, PANEL_BOT - 28, (char*)status);
	}
	else if (sel >= 0 && sel < pg->count &&
	         pg->items[sel].field == &menuConfig.video.bezel)		{
		int bz = menuConfig.video.bezel;
		if (bz > 0 && menuConfig.video.render != 2)		{
			oslSetTextColor(GB_LIGHTEST);
			oslDrawString(PANEL_X + 20, PANEL_BOT - 28,
			              "Only lines up with Screen size: Fit");
		}
		else if (bz >= 0 && bz < 4)		{
			oslSetTextColor(GB_LIGHT);
			oslDrawString(PANEL_X + 20, PANEL_BOT - 28, (char*)bezelDescs[bz]);
		}
	}
	else if (sel >= 0 && sel < pg->count &&
	         pg->items[sel].field == &menuConfig.video.overlay)		{
		//The overlays are drawn for the Fit viewport, so they only line up in Fit.
		//Say so rather than forcing the scaling - it is the player's choice.
		int ov = menuConfig.video.overlay;
		if (ov > 0 && menuConfig.video.render != 2)		{
			oslSetTextColor(GB_LIGHTEST);
			oslDrawString(PANEL_X + 20, PANEL_BOT - 28,
			              "Only lines up with Screen size: Fit");
		}
		else if (ov >= 0 && ov < 6)		{
			oslSetTextColor(GB_LIGHT);
			oslDrawString(PANEL_X + 20, PANEL_BOT - 28, (char*)filterDescs[ov]);
		}
	}
	else if (sel >= 0 && sel < pg->count && pg->items[sel].desc)		{
		oslSetTextColor(GB_LIGHT);
		oslDrawString(PANEL_X + 20, PANEL_BOT - 28, (char*)pg->items[sel].desc);
	}

	oslSetTextColor(GB_DARK);
	oslDrawString(PANEL_X + 20, PANEL_BOT - 14, "X select  O back  [] default");
}

//--- actions -----------------------------------------------------------------

static int DoAction(int action, char *status, int size)
{
	status[0] = '\0';
	switch (action)		{
		case A_RESUME:
			return 1;
		case A_SAVESTATE:
			snprintf(status, size, pspSaveState(stateSlot)
			         ? "Saved to slot %i" : "Could not save slot %i", stateSlot);
			break;
		case A_LOADSTATE:
			snprintf(status, size, pspLoadState(stateSlot)
			         ? "Loaded slot %i" : "No state in slot %i", stateSlot);
			break;
		case A_SAVENOW:
			//force = 1: write even if the CRC says nothing changed
			machine_manage_sram(SRAM_SAVE, 1);
			strncpy(status, "Game saved", size - 1);
			break;
		case A_DEFAULTS_ALL:		{
			//Settings go back; the loaded ROM stays loaded
			char rom[MAX_PATH];
			strcpy(rom, menuConfig.file.filename);
			memcpy(&menuConfig, menuConfigUserDefault, sizeof(MENUPARAMS));
			strcpy(menuConfig.file.filename, rom);
			ScalingPull();
			SaveUserDefaultConfig();
			strncpy(status, "All settings reset", size - 1);
			break;
		}
		case A_DELETE_AUTO:		{
			char statePath[MAX_PATH];
			pspGetStateNameEx(menuConfig.file.filename, statePath, STATE_AUTO);
			if (sceIoRemove(statePath) >= 0)
				strncpy(status, "Resume state deleted", size - 1);
			else
				strncpy(status, "No resume state", size - 1);
			break;
		}
		case A_BACK:
			//handled by the caller, which owns the page stack
			break;
		case A_RESET:
			machine_reset();
			return 1;
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
		oslDrawString(bx + 18, by + 16, "Continue where you left off?");

		oslSetTextColor(yes ? GB_LIGHTEST : GB_DARK);
		oslDrawString(bx + 42, by + 52, "Resume");
		oslSetTextColor(yes ? GB_DARK : GB_LIGHTEST);
		oslDrawString(bx + 138, by + 52, "New");
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

//Shown once, after the ROM is loaded and any snapshot restored, before the first
//frame of play. Two jobs: say whether this is a continue or a fresh start, and let
//the player press a button when they are ready rather than being dropped straight
//into the middle of a level.
void GameMenuStartGate(int resumed)
{
	int done = 0, frame = 0;

	LoadCursor();
	SfxInit();
	oslSetFramerate(60);
	osl_keys->pressed.value = 0;

	//Let go of whatever launched us first
	while (!osl_quit && (osl_keys->held.value & 0xf80f3f9))
		MyReadKeys();

	while (!osl_quit && !done)		{
		int bx = 96, by = 92, bw = 288, bh = 92;
		const char *line = resumed ? "Continuing where you left off"
		                           : "Ready to play";

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
		oslDrawString(bx + 20, by + 16, "RISE OF THE PENGUINS GB");
		MyDrawFillRect(bx + 20, by + 32, bx + bw - 20, by + 33, GB_DARK);

		oslSetTextColor(GB_LIGHTEST);
		oslDrawString(bx + 20, by + 42, (char*)line);

		//Blink, so it reads as waiting for you rather than frozen
		if ((frame / 30) & 1)
			oslSetTextColor(GB_LIGHT);
		else
			oslSetTextColor(GB_DARK);
		oslDrawString(bx + 20, by + 66, "Press any button to continue");

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

//Where the menu was when it last closed. Kept between openings so that changing a
//setting, closing to look at it and opening again lands back on the same row
//instead of at the top of the main page every time.
static int savedPage = P_MAIN, savedSel = 0, savedScroll = 0, savedDepth = 0;
static int savedStackPage[4], savedStackSel[4], savedStackScroll[4];

void GameMenuShow(void)
{
	int page, sel, scroll, quit = 0, statusTime = 0;
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
	ScalingPull();
	PaletteScan();
	PalettePull();

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
			strcpy(status, "Reverted to default");
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
				quit = DoAction(it->arg, status, sizeof(status));
				statusTime = 120;
			}
			else if (it->kind == K_KEY)		{
				u32 k = CaptureKey(page, sel, scroll, it->label);
				if (k)
					menuConfig.ctrl.akeys[it->arg] = k;
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
	osl_keys->pressed.value = 0;
	//The dismissing press is still held; keep it out of the game for up to two
	//seconds, or until the pad is released
	gblSwallowInput = 120;
}
