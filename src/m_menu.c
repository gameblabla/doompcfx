/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *  DOOM selection menu, options, episode etc. (aka Big Font menus)
 *  Sliders and icons. Kinda widget stuff.
 *  Setup Menus.
 *  Extended HELP screens.
 *  Dynamic HELP screen.
 *
 *-----------------------------------------------------------------------------*/

#include <stdio.h>
#include <fcntl.h>

#include "doomdef.h"
#include "doomstat.h"
#include "dstrings.h"
#include "d_main.h"
#include "v_video.h"
#include "w_wad.h"
#include "r_main.h"
#include "hu_stuff.h"
#include "g_game.h"
#include "s_sound.h"
#include "sounds.h"
#include "m_menu.h"
#include "m_misc.h"
#include "lprintf.h"
#include "am_map.h"
#include "i_main.h"
#include "i_system.h"
#include "i_video.h"
#include "i_sound.h"

#include "global_data.h"
#include "pcfx_text.h"    /* VDC BG font-tile overlay for menu text */

static void (*messageRoutine)(int response);

// we are going to be entering a savegame string

#define LINEHEIGHT  16


// end of externs added for setup menus

//
// PROTOTYPES
//
void M_NewGame(int choice);
void M_Episode(int choice);
void M_ChooseSkill(int choice);
void M_LoadGame(int choice);
void M_SaveGame(int choice);
void M_Options(int choice);


void M_ChangeMessages(int choice);
void M_ChangeAlwaysRun(int choice);
void M_ChangeGamma(int choice);
void M_ChangeMouseSensitivity(int choice);
void M_SfxVol(int choice);
void M_MusicVol(int choice);
void M_StartGame(int choice);
void M_Sound(int choice);

void M_LoadSelect(int choice);
void M_SaveSelect(int choice);
void M_ReadSaveStrings(void);
void M_QuickSave(void);
void M_QuickLoad(void);

void M_DrawMainMenu(void);
void M_DrawNewGame(void);
void M_DrawEpisode(void);
void M_DrawOptions(void);
void M_DrawSound(void);
void M_DrawLoad(void);
void M_DrawSave(void);

void M_SetupNextMenu(const menu_t *menudef);
void M_DrawThermo(int x,int y,int thermWidth,int thermDot);
void M_WriteText(int x, int y, const char *string);
int  M_StringWidth(const char *string);
int  M_StringHeight(const char *string);
void M_StartMessage(const char *string,void *routine,boolean input);
void M_ClearMenus (void);

// phares 3/30/98
// prototypes added to support Setup Menus and Extended HELP screens

void M_Setup(int choice);


// end of prototypes added to support Setup Menus and Extended HELP screens

/////////////////////////////////////////////////////////////////////////////
//
// PC-FX uniform menu rendering
//
// The stock menus mixed big 320x200 graphic lumps (M_DOOM, M_NGAME, M_OPTTTL,
// the ON/OFF and thermometer widgets, ...) with small hu_font text, drawn at
// their 320-space coordinates on the 256px screen -> off-centre, uneven, and
// blank wherever the trimmed IWAD lacks a lump. Everything is now drawn as
// hu_font text, horizontally centred on the 256px screen, so titles, items and
// values share one uniform look. All item/title drawing goes through the two
// helpers below; M_DrawItem also records the selected row so M_Drawer can place
// the skull cursor just left of it regardless of the item's width.
//
#define MENU_CX 128   // screen centre in native px (256/2)

static int s_selX, s_selY;   // left edge + y of the selected item (skull anchor)

// Draw one menu row of text, horizontally centred. If it is the selected item,
// record its anchor for the cursor.
static void M_DrawItem(int index, int y, const char *text)
{
    int x = MENU_CX - M_StringWidth(text) / 2;
    M_WriteText(x, y, text);
    if (index == _g->itemOn)
    {
        s_selX = x;
        s_selY = y;
    }
}

// Draw one menu row left-aligned at x (records the cursor anchor like M_DrawItem).
// Used by the main menu so its rows share one left edge instead of each being
// individually centred (which left "NEW GAME"/"OPTIONS" out of line with the
// wider "LOAD GAME"/"SAVE GAME").
static void M_DrawItemLeft(int index, int x, int y, const char *text)
{
    M_WriteText(x, y, text);
    if (index == _g->itemOn)
    {
        s_selX = x;
        s_selY = y;
    }
}

// Draw a centred menu title.
static void M_DrawTitle(int y, const char *text)
{
    M_WriteText(MENU_CX - M_StringWidth(text) / 2, y, text);
}

/////////////////////////////////////////////////////////////////////////////
//
// DOOM MENUS
//

/////////////////////////////
//
// MAIN MENU
//

// main_e provides numerical values for which Big Font screen you're on

enum
{
  newgame = 0,
  loadgame,
  savegame,
  options,
  main_end
};

//
// MainMenu is the definition of what the main menu Screen should look
// like. Each entry shows that the cursor can land on each item (1), the
// built-in graphic lump (i.e. "M_NGAME") that should be displayed,
// the program which takes over when an item is selected, and the hotkey
// associated with the item.
//

static const menuitem_t MainMenu[]=
{
  {1,"NEW GAME", M_NewGame},
  {1,"OPTIONS",  M_Options},
  {1,"LOAD GAME",M_LoadGame},
  {1,"SAVE GAME",M_SaveGame},
};

static const menu_t MainDef =
{
  main_end,       // number of menu items
  MainMenu,       // table that defines menu items
  M_DrawMainMenu, // drawing routine
  97,64,          // initial cursor position
  NULL,0,
};

//
// M_DrawMainMenu
//

void M_DrawMainMenu(void)
{
  // No title graphic: the TITLEPIC already shows the DOOM logo behind this menu,
  // and drawing M_DOOM on top of it doubled/mis-centred the logo. In-game (pause)
  // the four items are self-explanatory. Items are drawn by M_Drawer.
}

/////////////////////////////
//
// EPISODE SELECT
//

//
// episodes_e provides numbers for the episode menu items. The default is
// 4, to accomodate Ultimate Doom. If the user is running anything else,
// this is accounted for in the code.
//

enum
{
  ep1,
  ep2,
  ep3,
  ep4,
  ep_end
};


// The definitions of the Registered/Shareware Episodes menu

static const menuitem_t EpisodeMenu3[]=
{
  {1,"EPISODE 1", M_Episode},
  {1,"EPISODE 2", M_Episode},
  {1,"EPISODE 3", M_Episode}
};

static const menu_t EpiDef3 =
{
  ep_end-1,        // # of menu items
  EpisodeMenu3,   // menuitem_t ->
  M_DrawEpisode, // drawing routine ->
  48,63,         // x,y
  &MainDef,0,
};

// The definitions of the Episodes menu

static const menuitem_t EpisodeMenu[]=
{
  {1,"EPISODE 1", M_Episode},
  {1,"EPISODE 2", M_Episode},
  {1,"EPISODE 3", M_Episode},
  {1,"EPISODE 4", M_Episode}
};

static const menu_t EpiDef =
{
  ep_end,        // # of menu items
  EpisodeMenu,   // menuitem_t ->
  M_DrawEpisode, // drawing routine ->
  48,63,         // x,y
  &MainDef,0,
};

// numerical values for the New Game menu items

enum
{
  killthings,
  toorough,
  hurtme,
  violence,
  nightmare,
  newg_end
};

// The definitions of the New Game menu

/* PC-FX: skill labels drawn as text in M_DrawNewGame (empty names) so they
 * never depend on the M_JKILL..M_NMARE graphics lumps being present. */
static const menuitem_t NewGameMenu[]=
{
  {1,"", M_ChooseSkill},
  {1,"", M_ChooseSkill},
  {1,"", M_ChooseSkill},
  {1,"", M_ChooseSkill},
  {1,"", M_ChooseSkill}
};

static const char *const skillText[5] =
{
  "I'M TOO YOUNG TO DIE.",
  "HEY, NOT TOO ROUGH.",
  "HURT ME PLENTY.",
  "ULTRA-VIOLENCE.",
  "NIGHTMARE!"
};

static const menu_t NewDef =
{
  newg_end,       // # of menu items
  NewGameMenu,    // menuitem_t ->
  M_DrawNewGame,  // drawing routine ->
  48,63,          // x,y
  &MainDef,0,
};

//
//    M_Episode
//

void M_DrawEpisode(void)
{
  M_DrawTitle(24, "WHICH EPISODE?");
  // Episode items are drawn by M_Drawer.
}

void M_Episode(int choice)
{
  if ( (_g->gamemode == shareware) && choice)
  {
    M_StartMessage(SWSTRING,NULL,false); // Ty 03/27/98 - externalized
    _g->itemOn = 0;
    return;
  }

  // Yet another hack...
  if ( (_g->gamemode == registered) && (choice > 2))
    {
    lprintf( LO_WARN,
     "M_Episode: 4th episode requires UltimateDOOM\n");
    choice = 0;
    }

  _g->epi = choice;
  M_SetupNextMenu(&NewDef);
  _g->itemOn = 2; //Set hurt me plenty as default difficulty
}

//
// M_NewGame
//

void M_DrawNewGame(void)
{
  M_DrawTitle(24, "CHOOSE SKILL LEVEL");

  // The five skill labels as centred text (see NewGameMenu).
  for (int i = 0; i < newg_end; i++)
    M_DrawItem(i, NewDef.y + LINEHEIGHT * i, skillText[i]);
}

void M_NewGame(int choice)
{
    if ( _g->gamemode == commercial )
    {
		M_SetupNextMenu(&NewDef);
		_g->itemOn = 2; //Set hurt me plenty as default difficulty
	}else if( _g->gamemode == retail )
        M_SetupNextMenu(&EpiDef);       // only Ultimate DOOM has M_EPI4
    else
        M_SetupNextMenu(&EpiDef3);      // shareware/registered/unknown: 3-episode menu, no M_EPI4
}

// CPhipps - static
static void M_VerifyNightmare(int ch)
{
    if (ch != key_enter)
        return;

    G_DeferedInitNew(nightmare,_g->epi+1,1);
}

void M_ChooseSkill(int choice)
{
    if (choice == nightmare)
    {   // Ty 03/27/98 - externalized
        M_StartMessage(NIGHTMARE,M_VerifyNightmare,true);
		_g->itemOn = 0;
    }
    else
    {
        G_DeferedInitNew(choice,_g->epi+1,1);
		M_ClearMenus ();
    }    
}

/////////////////////////////
//
// LOAD GAME MENU
//

// numerical values for the Load Game slots

enum
{
    load1,
    load2,
    load3,
    load4,
    load5,
    load6,
    load7,
    load8,
    load_end
};

// The definitions of the Load Game screen

static const menuitem_t LoadMenue[]=
{
    {1,"", M_LoadSelect},
    {1,"", M_LoadSelect},
    {1,"", M_LoadSelect},
    {1,"", M_LoadSelect},
    {1,"", M_LoadSelect},
    {1,"", M_LoadSelect},
    {1,"", M_LoadSelect},
    {1,"", M_LoadSelect},
};

static const menu_t LoadDef =
{
  load_end,
  LoadMenue,
  M_DrawLoad,
  64,34, //jff 3/15/98 move menu up
  &MainDef,2,
};

//
// M_LoadGame & Cie.
//

void M_DrawLoad(void)
{
    int i;

    M_DrawTitle(16, "LOAD GAME");

    for (i = 0 ; i < load_end ; i++)
    {
        const char *s = _g->savegamestrings[i][0] ? _g->savegamestrings[i] : "- EMPTY -";
        M_DrawItem(i, LoadDef.y + LINEHEIGHT*i, s);
    }
}

//
// User wants to load this game
//

void M_LoadSelect(int choice)
{
  // CPhipps - Modified so savegame filename is worked out only internal
  //  to g_game.c, this only passes the slot.

  G_LoadGame(choice, false); // killough 3/16/98, 5/15/98: add slot, cmd

  M_ClearMenus ();
}

//
// Selected from DOOM menu
//

void M_LoadGame (int choice)
{
  /* killough 5/26/98: exclude during demo recordings
   * cph - unless a new demo */

  M_SetupNextMenu(&LoadDef);
  M_ReadSaveStrings();
}

/////////////////////////////
//
// SAVE GAME MENU
//

// The definitions of the Save Game screen

const static menuitem_t SaveMenu[]=
{
  {1,"", M_SaveSelect},
  {1,"", M_SaveSelect},
  {1,"", M_SaveSelect},
  {1,"", M_SaveSelect},
  {1,"", M_SaveSelect},
  {1,"", M_SaveSelect},
  {1,"", M_SaveSelect}, //jff 3/15/98 extend number of slots
  {1,"", M_SaveSelect},
};

const static menu_t SaveDef =
{
  load_end, // same number of slots as the Load Game screen
  SaveMenu,
  M_DrawSave,
  80,34, //jff 3/15/98 move menu up
  &MainDef,3,
};

//
// M_ReadSaveStrings
//  read the strings from the savegame files
//
void M_ReadSaveStrings(void)
{

}

//
//  M_SaveGame & Cie.
//
void M_DrawSave(void)
{
    int i;

    M_DrawTitle(16, "SAVE GAME");

    for (i = 0 ; i < load_end ; i++)
    {
        const char *s = _g->savegamestrings[i][0] ? _g->savegamestrings[i] : "- EMPTY -";
        M_DrawItem(i, SaveDef.y + LINEHEIGHT*i, s);
    }
}

//
// M_Responder calls this when user is finished
//
static void M_DoSave(int slot)
{
  G_SaveGame (slot,_g->savegamestrings[slot]);
  M_ClearMenus ();
}

//
// User wants to save. Start string input for M_Responder
//
void M_SaveSelect(int choice)
{
    _g->saveSlot = choice;

    M_DoSave(_g->saveSlot);
}

//
// Selected from DOOM menu
//
void M_SaveGame (int choice)
{
  // killough 10/6/98: allow savegames during single-player demo playback
  if (!_g->usergame && (!_g->demoplayback))
    {
    M_StartMessage(SAVEDEAD,NULL,false); // Ty 03/27/98 - externalized
    return;
    }

  if (_g->gamestate != GS_LEVEL)
    return;

  M_SetupNextMenu(&SaveDef);
  M_ReadSaveStrings();
}

/////////////////////////////
//
// OPTIONS MENU
//

// numerical values for the Options menu items

enum
{                                                   // phares 3/21/98
  messages,
  alwaysrun,
  gamma,
  mousesens,
  soundvol,
  opt_end
};

// The definitions of the Options menu

// All labels + values are drawn as centred text in M_DrawOptions (empty names,
// so M_Drawer's generic loop skips them). This keeps the whole menu uniform.
static const menuitem_t OptionsMenu[]=
{
  // killough 4/6/98: move setup to be a sub-menu of OPTIONs
  {1,"", M_ChangeMessages},
  {1,"", M_ChangeAlwaysRun},
  {2,"", M_ChangeGamma},
  {2,"", M_ChangeMouseSensitivity},
  {1,"", M_Sound}
};

const static menu_t OptionsDef =
{
  opt_end,
  OptionsMenu,
  M_DrawOptions,
  60,37,
  &MainDef,1,
};

//
// M_Options
//
void M_DrawOptions(void)
{
  char buf[40];
  const int y = OptionsDef.y;

  M_DrawTitle(16, "OPTIONS");

  sprintf(buf, "MESSAGES: %s", _g->showMessages ? "ON" : "OFF");
  M_DrawItem(messages, y + LINEHEIGHT*messages, buf);

  sprintf(buf, "ALWAYS RUN: %s", _g->alwaysRun ? "ON" : "OFF");
  M_DrawItem(alwaysrun, y + LINEHEIGHT*alwaysrun, buf);

  sprintf(buf, "GAMMA BOOST: %d", _g->gamma);
  M_DrawItem(gamma, y + LINEHEIGHT*gamma, buf);

  sprintf(buf, "MOUSE SENS: %d", _g->mouseSensitivity);
  M_DrawItem(mousesens, y + LINEHEIGHT*mousesens, buf);

  M_DrawItem(soundvol, y + LINEHEIGHT*soundvol, "SOUND VOLUME");
}

void M_Options(int choice)
{
  M_SetupNextMenu(&OptionsDef);
}

/////////////////////////////
//
// SOUND VOLUME MENU
//

// numerical values for the Sound Volume menu items
// The 'empty' slots are where the sliding scales appear.

enum
{
  sfx_vol,
  sfx_empty1,
  music_vol,
  sfx_empty2,
  sound_end
};

// The definitions of the Sound Volume menu

static const menuitem_t SoundMenu[]=
{
  // Labels + values are drawn as centred text in M_DrawSound (empty names,
  // like OptionsMenu), so the generic loop in M_Drawer doesn't re-draw the
  // raw lump names over them or steal the skull-cursor anchor.
  {2,"",M_SfxVol},
  {-1,"",0},
  {2,"",M_MusicVol},
  {-1,"",0}
};

static const menu_t SoundDef =
{
  sound_end,
  SoundMenu,
  M_DrawSound,
  80,64,
  &OptionsDef,4,
};

//
// Change Sfx & Music volumes
//

void M_DrawSound(void)
{
  char buf[32];

  M_DrawTitle(24, "SOUND VOLUME");

  sprintf(buf, "SFX VOLUME: %d", _g->snd_SfxVolume);
  M_DrawItem(sfx_vol, SoundDef.y + LINEHEIGHT*sfx_vol, buf);

  sprintf(buf, "MUSIC VOLUME: %d", _g->snd_MusicVolume);
  M_DrawItem(music_vol, SoundDef.y + LINEHEIGHT*music_vol, buf);
}

void M_Sound(int choice)
{
  M_SetupNextMenu(&SoundDef);
}

void M_SfxVol(int choice)
{
  switch(choice)
    {
    case 0:
      if (_g->snd_SfxVolume)
        _g->snd_SfxVolume--;
      break;
    case 1:
      if (_g->snd_SfxVolume < 15)
        _g->snd_SfxVolume++;
      break;
    }

  G_SaveSettings();

  S_SetSfxVolume(_g->snd_SfxVolume /* *8 */);
}

void M_MusicVol(int choice)
{
  switch(choice)
    {
    case 0:
      if (_g->snd_MusicVolume)
        _g->snd_MusicVolume--;
      break;
    case 1:
      if (_g->snd_MusicVolume < 15)
        _g->snd_MusicVolume++;
      break;
    }

  G_SaveSettings();

  S_SetMusicVolume(_g->snd_MusicVolume /* *8 */);
}

/////////////////////////////
//
//    Toggle messages on/off
//

void M_ChangeMessages(int choice)
{
  // warning: unused parameter `int choice'
  choice = 0;
  _g->showMessages = 1 - _g->showMessages;

  if (!_g->showMessages)
    _g->player.message = MSGOFF; // Ty 03/27/98 - externalized
  else
    _g->player.message = MSGON ; // Ty 03/27/98 - externalized

  _g->message_dontfuckwithme = true;

  G_SaveSettings();
}


void M_ChangeAlwaysRun(int choice)
{
    // warning: unused parameter `int choice'
    choice = 0;
    _g->alwaysRun = 1 - _g->alwaysRun;

    if (!_g->alwaysRun)
      _g->player.message = RUNOFF; // Ty 03/27/98 - externalized
    else
      _g->player.message = RUNON ; // Ty 03/27/98 - externalized

    G_SaveSettings();
}

void M_ChangeGamma(int choice)
{
	switch(choice)
    {
		case 0:
		  if (_g->gamma)
			_g->gamma--;
		  break;
		case 1:
		  if (_g->gamma < 5)
			_g->gamma++;
		  break;
    }
	V_SetPalLump(_g->gamma);
	V_SetPalette(0);

    G_SaveSettings();
}

// Turn speed per PC-FX mouse count; see MOUSE_TURN_SCALE in doomdef.h.  The
// default suits the real mouse (~200 counts/inch); emulator players feeding a
// modern host mouse through the port will want this much lower.
void M_ChangeMouseSensitivity(int choice)
{
    switch(choice)
    {
        case 0:
          if (_g->mouseSensitivity)
            _g->mouseSensitivity--;
          break;
        case 1:
          if (_g->mouseSensitivity < MOUSE_SENS_MAX)
            _g->mouseSensitivity++;
          break;
    }

    G_SaveSettings();
}

//
// End of Original Menus
//
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////
//
// General routines used by the Setup screens.
//

//
// M_InitDefaults()
//
// killough 11/98:
//
// This function converts all setup menu entries consisting of cfg
// variable names, into pointers to the corresponding default[]
// array entry. var.name becomes converted to var.def.
//

static void M_InitDefaults(void)
{

}

//
// End of Setup Screens.
//
/////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////
//
// M_Responder
//
// Examines incoming keystrokes and button pushes and determines some
// action based on the state of the system.
//

boolean M_Responder (event_t* ev)
{
    int    ch;

    ch = -1; // will be changed to a legit char if we're going to use it here


    // Mouse input processing removed

    // Process keyboard input

    if (ev->type == ev_keydown)
    {
        ch = ev->data1;               // phares 4/11/98:
    }                             // down so you can get at the !,#,


    if (ch == -1)
        return false; // we can't use the event here

    // Take care of any messages that need input

    if (_g->messageToPrint)
    {
        if (_g->messageNeedsInput == true &&
                !(ch == ' ' || ch == 'n' || ch == 'y' || ch == key_escape || ch == key_fire || ch == key_enter)) // phares
            return false;

        _g->menuactive = _g->messageLastMenuActive;
        _g->messageToPrint = 0;
        if (messageRoutine)
            messageRoutine(ch);

        _g->menuactive = false;
        S_StartSound(NULL,sfx_swtchx);
        return true;
    }

    // Pop-up Main menu?

    if (!_g->menuactive)
    {
        if (ch == key_escape)                                     // phares
        {
            M_StartControlPanel ();
            S_StartSound(NULL,sfx_swtchn);
            return true;
        }
        return false;
    }

    // From here on, these navigation keys are used on the BIG FONT menus
    // like the Main Menu.

    if (ch == key_menu_down)                             // phares 3/7/98
    {
        do
        {
            if (_g->itemOn+1 > _g->currentMenu->numitems-1)
                _g->itemOn = 0;
            else
                _g->itemOn++;
            S_StartSound(NULL,sfx_pstop);
        }
        while(_g->currentMenu->menuitems[_g->itemOn].status==-1);
        return true;
    }

    if (ch == key_menu_up)                               // phares 3/7/98
    {
        do
        {
            if (!_g->itemOn)
                _g->itemOn = _g->currentMenu->numitems-1;
            else
                _g->itemOn--;
            S_StartSound(NULL,sfx_pstop);
        }
        while(_g->currentMenu->menuitems[_g->itemOn].status==-1);
        return true;
    }

    if (ch == key_menu_left)                             // phares 3/7/98
    {
        if (_g->currentMenu->menuitems[_g->itemOn].routine &&
                _g->currentMenu->menuitems[_g->itemOn].status == 2)
        {
            S_StartSound(NULL,sfx_stnmov);
            _g->currentMenu->menuitems[_g->itemOn].routine(0);
        }
        return true;
    }

    if (ch == key_menu_right)                            // phares 3/7/98
    {
        if (_g->currentMenu->menuitems[_g->itemOn].routine &&
                _g->currentMenu->menuitems[_g->itemOn].status == 2)
        {
            S_StartSound(NULL,sfx_stnmov);
            _g->currentMenu->menuitems[_g->itemOn].routine(1);
        }
        return true;
    }

    if (ch == key_menu_enter)                            // phares 3/7/98
    {
        if (_g->currentMenu->menuitems[_g->itemOn].routine &&
                _g->currentMenu->menuitems[_g->itemOn].status)
        {
            if (_g->currentMenu->menuitems[_g->itemOn].status == 2)
            {
                _g->currentMenu->menuitems[_g->itemOn].routine(1);   // right arrow
                S_StartSound(NULL,sfx_stnmov);
            }
            else
            {
                _g->currentMenu->menuitems[_g->itemOn].routine(_g->itemOn);
                S_StartSound(NULL,sfx_pistol);
            }
        }
        //jff 3/24/98 remember last skill selected
        // killough 10/98 moved to skill-specific functions
        return true;
    }

    if (ch == key_menu_escape)                           // phares 3/7/98
    {
        M_ClearMenus ();
        S_StartSound(NULL,sfx_swtchx);
        return true;
    }

	//Allow being able to go back in menus ~Kippykip
	if (ch == key_fire)                           // phares 3/7/98
    {
		//If the prevMenu == NULL (Such as main menu screen), then just get out of the menu altogether
		if(_g->currentMenu->prevMenu == NULL)
		{
			M_ClearMenus();
		}else //Otherwise, change to the parent menu and match the row used to get there.
		{
			short previtemOn = _g->currentMenu->previtemOn; //Temporarily store this so after menu change, we store the last row it was on.
			M_SetupNextMenu(_g->currentMenu->prevMenu);					
			_g->itemOn = previtemOn;
		}
        S_StartSound(NULL,sfx_swtchx);
        return true;		
    }
	
    return false;
}

//
// End of M_Responder
//
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
//
// General Routines
//
// This displays the Main menu and gets the menu screens rolling.
// Plus a variety of routines that control the Big Font menu display.
// Plus some initialization for game-dependant situations.

void M_StartControlPanel (void)
{
  // intro might call this repeatedly

  if (_g->menuactive)
    return;

  //jff 3/24/98 make default skill menu choice follow -skill or defaultskill
  //from command line or config file
  //
  // killough 10/98:
  // Fix to make "always floating" with menu selections, and to always follow
  // defaultskill, instead of -skill.

  _g->menuactive = 1;
  _g->currentMenu = &MainDef;         // JDC
}

//
// M_Drawer
// Called after the view has been rendered,
// but before it has been blitted.
//
// killough 9/29/98: Significantly reformatted source
//

void M_Drawer (void)
{
    // Horiz. & Vertically center string and print it.
    // killough 9/29/98: simplified code, removed 40-character width limit
    if (_g->messageToPrint)
    {
        /* cph - strdup string to writable memory */
        char *ms = Z_Strdup(_g->messageString);
        char *p = ms;

        int y = 80 - M_StringHeight(_g->messageString)/2;
        while (*p)
        {
            char *string = p, c;
            while ((c = *p) && *p != '\n')
                p++;
            *p = 0;
            M_WriteText(MENU_CX - M_StringWidth(string)/2, y, string);
            y += SHORT(_g->hu_font[0]->height);
            if ((*p = c))
                p++;
        }
        Z_Free(ms);
    }
    else
        if (_g->menuactive)
        {
            int y,max,i;

            s_selY = -1;    // M_DrawItem sets this to the selected row's anchor

            if (_g->currentMenu->routine)
                _g->currentMenu->routine();     // title + any value/dynamic items

            // Simple static items (main menu, episode select) are centred here;
            // value/dynamic menus draw their own items in the routine above. Both
            // go through M_DrawItem, so s_selX/s_selY end up anchored on the
            // selected row either way.
            y = _g->currentMenu->y;
            max = _g->currentMenu->numitems;

            // Main menu: left-align all rows to a common column, positioned so the
            // widest row is centred. This lines the four entries up on one left edge
            // instead of centring each individually. Other static menus (episode,
            // skill) keep the centred look.
            int leftx = -1;
            if (_g->currentMenu == &MainDef)
            {
                int mw = 0;
                for (i = 0; i < max; i++)
                {
                    int w = M_StringWidth(_g->currentMenu->menuitems[i].name);
                    if (w > mw) mw = w;
                }
                leftx = MENU_CX - mw / 2;
            }

            for (i=0;i<max;i++)
            {
                if (_g->currentMenu->menuitems[i].name[0])
                {
                    if (leftx >= 0)
                        M_DrawItemLeft(i, leftx, y, _g->currentMenu->menuitems[i].name);
                    else
                        M_DrawItem(i, y, _g->currentMenu->menuitems[i].name);
                }
                y += LINEHEIGHT;
            }

            // Cursor, two cells left of the selected (centred) item. Drawn on the
            // VDC text overlay (like the item text) with the font's '>' glyph — a
            // right-pointing arrow — so it never touches the KING framebuffer. That
            // keeps the title/background a single cached image with no per-frame
            // repaint (the old skull patch drew into the framebuffer and left a
            // trail, which forced a full-page RAM re-blit every menu frame).
            if (s_selY >= 0)
                pcfx_text_putc(s_selX / 8 - 2, s_selY / 8, '>');
        }
}

//
// M_ClearMenus
//
// Called when leaving the menu screens for the real world

void M_ClearMenus (void)
{
  _g->menuactive = 0;
  _g->itemOn = 0;
}

//
// M_SetupNextMenu
//
void M_SetupNextMenu(const menu_t *menudef)
{
  _g->currentMenu = menudef;
  _g->itemOn = 0;
}

/////////////////////////////
//
// M_Ticker
//
void M_Ticker (void)
{
  if (--_g->skullAnimCounter <= 0)
    {
      _g->whichSkull ^= 1;
      _g->skullAnimCounter = 8;
    }
}

/////////////////////////////
//
// Message Routines
//

void M_StartMessage (const char* string,void* routine,boolean input)
{
  _g->messageLastMenuActive = _g->menuactive;
  _g->messageToPrint = 1;
  _g->messageString = string;
  messageRoutine = routine;
  _g->messageNeedsInput = input;
  _g->menuactive = true;
  return;
}


/////////////////////////////
//
// Thermometer Routines
//

//
// M_DrawThermo draws the thermometer graphic for Mouse Sensitivity,
// Sound Volume, etc.
//
// NOTE: currently UNUSED — the menus render volumes/gamma as uniform text
// (see M_DrawOptions/M_DrawSound). Kept for future mouse-sensitivity support.
//
// proff/nicolas 09/20/98 -- changed for hi-res
// CPhipps - patch drawing updated
//
void M_DrawThermo(int x,int y,int thermWidth,int thermDot )
{
    int          xx;
    int           i;
    /*
   * Modification By Barry Mead to allow the Thermometer to have vastly
   * larger ranges. (the thermWidth parameter can now have a value as
   * large as 200.      Modified 1-9-2000  Originally I used it to make
   * the sensitivity range for the mouse better. It could however also
   * be used to improve the dynamic range of music and sound affect
   * volume controls for example.
   */
    int horizScaler; //Used to allow more thermo range for mouse sensitivity.
    thermWidth = (thermWidth > 200) ? 200 : thermWidth; //Clamp to 200 max
    horizScaler = (thermWidth > 23) ? (200 / thermWidth) : 8; //Dynamic range
    xx = x;

    int thermm_lump = W_GetNumForName("M_THERMM");

    V_DrawNamePatch(xx, y, 0, "M_THERML", CR_DEFAULT, VPT_STRETCH);

    xx += 8;
    for (i=0;i<thermWidth;i++)
    {
        V_DrawNumPatch(xx, y, 0, thermm_lump, CR_DEFAULT, VPT_STRETCH);
        xx += horizScaler;
    }

    xx += (8 - horizScaler);  /* make the right end look even */

    V_DrawNamePatch(xx, y, 0, "M_THERMR", CR_DEFAULT, VPT_STRETCH);
    V_DrawNamePatch((x+8)+thermDot*horizScaler,y,0,"M_THERMO",CR_DEFAULT,VPT_STRETCH);
}

/////////////////////////////
//
// String-drawing Routines
//

//
// Find string width from hu_font chars
//

// PC-FX: menu text is drawn on the VDC font-tile overlay at a fixed 8px cell pitch
// (monospace), so width is just the character count x 8 -- this keeps M_WriteText's
// cell placement and the menu's centring (MENU_CX - width/2) consistent.
int M_StringWidth(const char* string)
{
  int w = 0;
  for (int i = 0; string[i]; i++)
    if (string[i] != '\n')
      w += 8;
  return w;
}

//
//    Find string height from hu_font chars
//

int M_StringHeight(const char* string)
{
  int i, h, height = h = SHORT(_g->hu_font[0]->height);
  for (i = 0;string[i];i++)            // killough 1/31/98
    if (string[i] == '\n')
      h += height;
  return h;
}

//
//    Write a string using the hu_font
//
// PC-FX: place each glyph on the VDC font-tile overlay at cell (cx/8, cy/8) with a
// fixed 8px monospace advance. Vertical layout stays in pixels (12px line spacing) so
// menu row positions are unchanged; only the horizontal metric becomes monospace
// (matching M_StringWidth). Non-printable / space cells are left blank.
void M_WriteText (int x,int y,const char* string)
{
    const char* ch = string;
    int cx = x, cy = y, c;

    while ((c = *ch++)) {
        if (c == '\n') { cx = x; cy += 12; continue; }
        pcfx_text_putc(cx / 8, cy / 8, toupper(c));   // no-op for space/non-printable
        cx += 8;
    }
}

/////////////////////////////
//
// Initialization Routines to take care of one-time setup
//

//
// M_Init
//
void M_Init(void)
{
  M_InitDefaults();                // killough 11/98
  _g->currentMenu = &MainDef;
  _g->menuactive = 0;
  _g->whichSkull = 0;
  _g->skullAnimCounter = 10;
  _g->messageToPrint = 0;
  _g->messageString = NULL;
  _g->messageLastMenuActive = _g->menuactive;

  G_UpdateSaveGameStrings();
}

//
// End of General Routines
//
/////////////////////////////////////////////////////////////////////////////
