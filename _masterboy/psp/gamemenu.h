#ifndef _GAMEMENU_H_
#define _GAMEMENU_H_

//=== IN-GAME MENU ===
//A self-contained replacement for MasterBoy's rotating carousel, so the launcher
//looks like the game rather than like the emulator it is built on. Deliberately
//written as its own module: menuplus.c is ~5000 lines of interdependent state and
//editing it in place proved fragile.
//
//menuPlusShowMenu() is still used once at boot, because that is where the emulator
//loads its config, skins and fonts - this menu reuses what it set up.

//Shows the menu. Returns when the player resumes or quits.
void GameMenuShow(void);

//Boot-time prompt for the auto resume state. Returns 1 to load it.
int GameMenuAskResume(void);

//Title hold before play starts. resumed != 0 when a snapshot was restored.
void GameMenuStartGate(int resumed);

#endif
