/* -*- mode: c; tab-width: 4; c-basic-offset: 4; c-file-style: "linux" -*- */
//
// Copyright (c) 2009-2011, Wei Mingzhi <whistler_wmz@users.sf.net>.
// Copyright (c) 2011-2026, SDLPAL development team.
// All rights reserved.
//
// This file is part of SDLPAL.
//
// SDLPAL is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "main.h"
#include "bridge.h"

VOID
PAL_GameMain(
   VOID
)
/*++
  Purpose:

    The game entry routine.

  Parameters:

    None.

  Return value:

    None.

--*/
{
   PAL_BridgeMark(3);
   DWORD       dwTime;

   //
   // Show the opening menu.
   //
   // USER 22:25: a resumed run must NOT replay the opening. The agent
   // used to press through PAL_OpeningMenu() (0 = new game) and the whole
   // opening cutscene, then load the checkpoint -- so every window
   // restart visibly started a brand new game. Boot directly from the
   // save slot instead; PAL_ReloadInNextTick() then loads it on the
   // first frame of the ordinary main loop.
   {
      // USER「等下就从 278 开始打」：启动槽与 bridge save/load 同口径
      // 放宽到 < 1000，否则 PAL_AGENT_BOOT_SLOT=278 会被判成新游戏。
      INT iBoot = PAL_BridgeBootSlot();
      gpGlobals->bCurrentSaveSlot =
         ((iBoot > 0 && iBoot < 1000) ? iBoot : PAL_OpeningMenu());
   }
   gpGlobals->fInMainGame = TRUE;

   //
   // Initialize game data and set the flags to load the game resources.
   //
   PAL_ReloadInNextTick(gpGlobals->bCurrentSaveSlot);

   //
   // Run the main game loop.
   //
   dwTime = SDL_GetTicks();
   while (TRUE)
   {
      //
      // Load the game resources if needed.
      //
      PAL_LoadResources();

      //
      // Clear the input state of previous frame.
      //
      PAL_ClearKeyState();

      //
      // Wait for the time of one frame. Accept input here.
      //
      PAL_DelayUntil(dwTime);

      //
      // Set the time of the next frame.
      //
      dwTime = SDL_GetTicks() + FRAME_TIME;

      //
      // Run the main frame routine.
      //
      PAL_StartFrame();
   }
}
