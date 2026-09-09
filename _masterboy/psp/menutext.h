#ifndef _MENUTEXT_H_
#define _MENUTEXT_H_

//=== MENU TEXT ===
//NOT named strings.h: the build has -Ipsp on the include path, so a header of that
//name here shadows the system <strings.h> and gym.h loses strcasecmp.
//The launcher ships one build per language (roms/game_de.gb, game_fr.gb, ...) but
//the menu itself was English in every one of them. Translating it the usual way -
//turning every label into a numeric id - would have meant rewriting ~150 table
//entries in gamemenu.c, so this works the way gettext does instead: the English
//text stays in the tables and stays the lookup key.
//
//Two things fall out of that which matter. A string with no translation renders in
//English rather than as a blank or a wrong row, so a half-finished language is
//still shippable; and the tables in gamemenu.c remain readable as English.

//Picks the language from the ROM's filename - the packaging already encodes it
//there ("game_de.gb"), so this needs no new config field and no packaging change.
void MenuStringsInit(const char *romPath);

//Two-letter code of the active language, "en" when nothing matched. For diagnostics.
const char *MenuStringsLanguage(void);

//English in, translated out. Returns its argument unchanged when there is no
//translation, so it is always safe to wrap a string in this.
const char *Tr(const char *en);

#endif
