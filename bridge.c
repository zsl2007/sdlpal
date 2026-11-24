/* -*- mode: c; tab-width: 4; c-basic-offset: 4; c-file-style: "linux" -*- */
//
// bridge.c -- the agent bridge (see bridge.h).
//
// Wire protocol, newline-delimited JSON, engine = client:
//
//   engine -> agent : {"t":"hello","ver":1,"win95":0|1,"files":[{"n":..,"s":..}]}
//   agent  -> engine: {"seed":<n>,"fast":0|1,"ok":1}
//   engine -> agent : {"t":"state", ...snapshot... }        (queue is empty)
//   agent  -> engine: {"a":[{"k":"up","h":1},{"k":"-","h":30}, ...]}
//                     or {"a":"quit"}
//
// One action = "hold key k for h engine input-pumps". "-" means no key, which
// is how the agent lets an animation play out.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "bridge.h"
#include "global.h"
#include "palcommon.h"
#include "input.h"
#include "battle.h"
#include "palcfg.h"
#include "map.h"

// magicmenu.c accessors: the spell-selection cursor is a static local there.
extern int  PAL_MagicMenuCursor(VOID);
extern int  PAL_MagicMenuCount(VOID);
extern WORD PAL_MagicMenuSpell(int i);
// mnum alone is a stale static after the menu closes; mopen is the
// real "synchronous modal is running" flag (WORLD-phase supply could
// otherwise leave it open and freeze the party with ctl=0 forever).
extern int  PAL_MagicMenuIsOpen(VOID);
// uigame.c accessors: which party member the use-item menu points at.
extern int  PAL_ItemUseMenuCursor(VOID);
extern WORD PAL_ItemUseMenuRole(VOID);
extern int  PAL_ItemUseMenuActive(VOID);

//
// common.h aliases SDL_GetTicks to the bridge's virtual clock; the bridge
// itself still needs the real one to fall back on when it is disabled.
//
#undef SDL_GetTicks
#undef SDL_Delay

#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#define PAL_SOCKET        int
#define PAL_INVALID_SOCK  (-1)
#define PAL_CLOSE(s)      close(s)
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#define PAL_SOCKET        SOCKET
#define PAL_INVALID_SOCK  INVALID_SOCKET
#define PAL_CLOSE(s)      closesocket(s)
#endif

#define BRIDGE_VERSION      1
#define BRIDGE_BUFSZ        (256 * 1024)
#define BRIDGE_MAX_ACTION   512

typedef struct tagBRIDGEACTION
{
   DWORD   dwKey;
   INT     nFrames;
} BRIDGEACTION;

static INT            g_fEnabled = 0;
static INT            g_fFast = 0;
static INT            g_fNoRender = 0;
static INT            g_fAudio = 0;
static INT            g_fCinematics = 0;
static INT            g_iBootSlot = 0;
static INT            g_nFrameMs = 0;
static PAL_SOCKET     g_sock = PAL_INVALID_SOCK;
/* USER 10:11 "游戏启动后监听 socket，agent 去连游戏，任意状态可接管":
   the GUI owns a LISTEN socket and keeps running standalone; an agent may
   attach at any time, take over the CURRENT in-memory state, detach, and be
   replaced by a fresh agent without ever restarting the game. */
static INT            g_fListen = 0;
static PAL_SOCKET     g_listenSock = PAL_INVALID_SOCK;
static INT            g_nFrame = 0;
static INT            g_nSeed = 1;

static BRIDGEACTION   g_rgActions[BRIDGE_MAX_ACTION];
static INT            g_nActions = 0;
static INT            g_iAction = 0;
static INT            g_fQuitRequested = 0;
static INT            g_nServed = 0;
static INT            g_nCleared = 0;
static INT            g_nUI = 0;
// equip menu (uigame.c PAL_EquipItemMenu mirrors its state here): the item
// being worn and the member the cursor is on
static INT            g_nEquipItem = 0;
static INT            g_nEquipSel = 0;
// the item list itself (itemmenu.c PAL_ItemSelectMenu): the use menu and
// the equip menu both start here, and icur is only meaningful inside it
static INT            g_fItemMenu = 0;
static INT            g_nWorldFrames = 0;
static INT            g_fConfirm = 0;
static INT            g_nDialog = 0;
static INT            g_fWaitKey = 0;
static INT            g_iDialogMsg = -1;

// USER 23:43 "能打开菜单 才能保存啊". A normal player can only save
// from the in-game system menu, and PAL_InGameMenu() is reached only at
// the TAIL of PAL_StartFrame() (play.c:551) -- i.e. after
// PAL_GameUpdate(TRUE) has fully returned. While a trigger script is
// running, in a battle, in a dialog/wait, or while a menu/shop/battle
// UI is modal, the world loop does not run and g_nWorldFrames stays 0
// (the "ctl" field). That is the engine's own, honest definition of
// "the save menu is reachable". The agent's {"save":N} used to call
// PAL_SaveGame() unconditionally, bypassing that menu entirely, so it
// could freeze a half-executed plot script into the .rpg file.
static INT            g_fSaveAllowed = 0;
// Incremented ONLY when PAL_SaveGame() actually ran. The agent compares
// this in consecutive reports to know whether its {"save":N} landed;
// otherwise a refused save would still advance CKPT_STATE and leave the
// world file and the rollback memory out of lockstep.
static INT            g_nSaveCount = 0;

// Maintained by script.c around every PAL_RunTriggerScript() call.
extern INT            g_nScriptDepth;
// script.c: 当前正在执行的脚本指令地址（SSS.MKF 条目下标），-1 = 无脚本
extern INT            g_iCurrentScriptAddr;
// script.c: wAutoScript（楼梯/自动搬运）每帧执行的指令地址，-1 = 无
extern INT            g_iAutoScriptAddr;
// script.c: 当前触发脚本由哪个对象触发（1-based；0/0xFFFF 无宿主；-1 无脚本）
extern INT            g_iCurrentEventObject;

// One-shot "a recovery load was requested while a synchronous script
// was waiting for a key". See palcommon.h for the full rationale.
// Set by {"load":N} in _bridge_parse(); consumed by whichever
// PAL_WaitForKey* / dialog-wait loop is currently blocking.
// USER「持续迭代目标通关」：a pre-battle script has dozens of dialog
// lines, EACH with its own PAL_DialogWaitForKey loop. A one-shot flag
// frees only the first loop; the next one blocks again and the engine
// never returns to game.c, so PAL_LoadResources() never consumes the
// reload flags. Give the recovery request a bounded frame budget so
// every wait loop in that script can break, then let it expire so
// ordinary dialogs are unaffected.
static INT            g_nBridgeRecoveryBudget = 0;
#define BRIDGE_RECOVERY_BUDGET 512

// Virtual clock, declared here (before _bridge_clock_domain_reset) so the
// attach/detach boundary helper can realign it. Was previously further
// down next to the SAID ring buffer.
static DWORD          g_dwTicks = 0;

//
// USER「agent 退出后按键无效」: the engine's SDL_GetTicks() is mapped to
// PAL_BridgeTicks() (common.h), which returns the VIRTUAL g_dwTicks while
// an agent is attached and the REAL wall clock after detach. input.c's
// keyboard debounce table is written with that clock, so leaving the
// virtual-clock domain makes "dwCurrentTime > rgdwKeyLastTime[i]" false
// forever and every physical keypress is swallowed. Reset the debounce
// table whenever we cross that boundary (attach AND detach), and align
// the virtual clock to the real one so no tick-based wait chases a stale
// deadline.
//
static VOID
_bridge_clock_domain_reset(
   VOID
)
{
   // bridge.c #undef'd SDL_GetTicks above, so this is the real clock.
   g_dwTicks = SDL_GetTicks();
   PAL_ResetKeyTimings();
   // USER「farm 与战斗策略不生效」：PAL_BridgeItemMenu(0) is only
   // called when the item-select menu returns normally. If a load /
   // detach interrupts it, g_fItemMenu stays latched at 1 forever,
   // so relaxWalk() and the BFS wait loop keep treating the world
   // as a modal item menu and never issue direction keys. Real run:
   // itm=true for 17064 batches at s6 (1840,1896) while the live
   // engine had already left the menu. Clear it at every
   // attach/detach boundary, same as the key debounce table.
   g_fItemMenu = 0;
}

static INT
_bridge_save_allowed(VOID)
{
   if (gpGlobals == NULL)
      return 0;
   if (gpGlobals->fInBattle || gpGlobals->fAutoBattle ||
       gpGlobals->fEnteringScene)
      return 0;
   // A plot script is running synchronously inside PAL_GameUpdate().
   // Its program counter is NOT stored in SAVEDGAME, so any save taken
   // here is unresumable. This is the exact "cannot open the save menu"
   // condition for a normal player.
   if (g_nScriptDepth > 0)
      return 0;
   // g_nUI is NOT a modal flag: PAL_BridgeMark() sets it once at boot
   // (game.c:44 -> 3) and never clears it. Use the real modal state:
   // dialog/wait (a running plot script always goes through
   // PAL_WaitForKey / PAL_StartDialog), confirm, item/equip menus.
   if (g_nDialog != 0 || g_fWaitKey || g_fConfirm ||
       g_fItemMenu || g_nEquipItem != 0)
      return 0;
   // ctl: the world loop ran since the last report. Any modal state
   // (menu, shop, dialog, cutscene, battle UI) leaves it 0.
   if (g_nWorldFrames <= 0)
      return 0;
   return 1;
}

//
// Every M.MSG line shown since the last report, in order. A sampled "is a
// dialog open" flag cannot capture what was said: item pickups use a
// center window that closes itself after 1.4 s (text.c, ShowDialogText), so
// by the time the batch ends the line is long gone and the conversation
// looked silent although the item was handed over. An event log cannot miss.
//
#define BRIDGE_MAX_SAID     64
static INT            g_rgSaid[BRIDGE_MAX_SAID];
static INT            g_nSaid = 0;

static char           g_szOut[BRIDGE_BUFSZ];
static char           g_szIn[BRIDGE_BUFSZ];
static size_t         g_nInLen = 0;

// ------------------------------------------------------------------ socket

// Forwarded: _bridge_parse() handles the "dump" command but the
// implementation sits with the rest of the dump code below.
static void _bridge_dump(VOID);
// Lightweight live-object refresh (same objs framing as the full dump).
static void _bridge_dump_objs_only(VOID);

static int
_bridge_send(
   const char *psz
)
{
   size_t n = strlen(psz), sent = 0;
   if (g_sock == PAL_INVALID_SOCK)
      return 0;
   while (sent < n)
   {
      int r = (int)send(g_sock, psz + sent, (int)(n - sent), 0);
      if (r <= 0)
         return 0;
      sent += (size_t)r;
   }
   return 1;
}

//
// Read one '\n' terminated line. Blocking: the agent is the scheduler, so the
// engine is allowed to sit here between decisions.
//
static char *
_bridge_recv_line(
   void
)
{
   while (1)
   {
      char *pszEnd = memchr(g_szIn, '\n', g_nInLen);
      if (pszEnd != NULL)
      {
         *pszEnd = '\0';
         static char szLine[65536];
         size_t n = strlen(g_szIn);
         if (n >= sizeof(szLine))
            n = sizeof(szLine) - 1;
         memcpy(szLine, g_szIn, n);
         szLine[n] = '\0';
         memmove(g_szIn, g_szIn + n + 1, g_nInLen - n - 1);
         g_nInLen -= n + 1;
         return szLine;
      }
      if (g_nInLen + 4096 > BRIDGE_BUFSZ)
         g_nInLen = 0;
      int r = (int)recv(g_sock, g_szIn + g_nInLen, 4096, 0);
      if (r <= 0)
         return NULL;
      g_nInLen += (size_t)r;
   }
}

static int
_json_int(
   const char *psz,
   const char *pszKey,
   int         nDefault
)
{
   char szKey[64];
   snprintf(szKey, sizeof(szKey), "\"%s\"", pszKey);
   const char *p = strstr(psz, szKey);
   if (p == NULL)
      return nDefault;
   p += strlen(szKey);
   while (*p && (*p == ' ' || *p == ':'))
      p++;
   return atoi(p);
}

// ------------------------------------------------------------------- RNG

extern int  PAL_BridgeSetSeedImpl(int iSeed);   // implemented in util.c

INT
PAL_BridgeSetSeed(
   INT iSeed
)
{
   g_nSeed = iSeed;
   return PAL_BridgeSetSeedImpl(iSeed);
}

// ------------------------------------------------------------------ state

//
// FNV-1a over everything that changes when the story advances: every event
// object's script pointers and state, the party roster and the inventory.
// A change in this value is one unit of plot progress by definition.
//
static unsigned long long
_bridge_fingerprint(
   void
)
{
   unsigned long long h = 1469598103934665603ULL;
   int i;

#define MIX(v) do { \
      unsigned long long _v = (unsigned long long)(unsigned)(v); \
      h ^= _v; h *= 1099511628211ULL; \
   } while (0)

   if (gpGlobals != NULL)
   {
      MIX(gpGlobals->wNumScene);
      MIX(gpGlobals->wMaxPartyMemberIndex);
      MIX(gpGlobals->dwCash);

      if (gpGlobals->g.lprgEventObject != NULL)
      {
         //
         // Only the objects that actually exist. MAX_EVENT_OBJECTS is an
         // upper bound (5500) and the table is allocated with the real
         // count (nEventObject); hashing past the end pulled uninitialised
         // memory into the fingerprint and made identical runs look
         // different.
         //
         int n = (int)gpGlobals->g.nEventObject;
         if (n <= 0 || n > MAX_EVENT_OBJECTS)
            n = MAX_EVENT_OBJECTS;
         for (i = 0; i < n; i++)
         {
            LPEVENTOBJECT p = &gpGlobals->g.lprgEventObject[i];
            MIX(p->wTriggerScript);
            MIX(p->wAutoScript);
            MIX(p->sState);
         }
      }

      for (i = 0; i < MAX_INVENTORY; i++)
      {
         MIX(gpGlobals->rgInventory[i].wItem);
         MIX(gpGlobals->rgInventory[i].nAmount);
      }
   }
#undef MIX
   return h;
}

static size_t
_bridge_state(
   char  *psz,
   size_t n
)
{
   size_t used = 0;
   int i, j;

#define APP(...) do { \
      int _r = snprintf(psz + used, n - used, __VA_ARGS__); \
      if (_r < 0) { return used; } \
      if ((size_t)_r >= n - used) { return n - 1; } \
      used += (size_t)_r; \
   } while (0)

   if (gpGlobals == NULL)
   {
      APP("{\"t\":\"state\",\"f\":%d,\"ready\":0}", g_nFrame);
      return used;
   }

   int x = PAL_X(gpGlobals->viewport) + PAL_X(gpGlobals->partyoffset);
   int y = PAL_Y(gpGlobals->viewport) + PAL_Y(gpGlobals->partyoffset);

   APP("{\"t\":\"state\",\"f\":%d,\"ready\":1", g_nFrame);
   APP(",\"scene\":%d,\"map\":%d", (int)gpGlobals->wNumScene,
       (int)gpGlobals->g.rgScene[gpGlobals->wNumScene - 1].wMapNum);
   APP(",\"party\":[%d,%d],\"dir\":%d", x, y, (int)gpGlobals->wPartyDirection);
   APP(",\"cash\":%d", (int)gpGlobals->dwCash);
   // inventory menu cursor: lets the agent drive the real item menu by keys
   APP(",\"icur\":%d", (int)gpGlobals->iCurInvMenuItem);
   // equip menu (PAL_EquipItemMenu): the item being worn and the member the
   // cursor is on; 0 when the menu is closed
   APP(",\"eqm\":%d,\"eqs\":%d", g_nEquipItem, g_nEquipSel);
   // pcur/prole = the party-member cursor in PAL_ItemUseMenu and the
   // role id it points at. 0xFFFF means no valid member. A single-target
   // medicine must be applied to the role the planner picked, not to
   // whatever static sSelectedPlayer happens to hold from last time.
   // puse = the party-select menu is open. Unlike itm (item-select menu)
   // it is a separate synchronous modal; leaving it open swallows every
   // direction key and freezes the party after a supply action.
   APP(",\"pcur\":%d,\"prole\":%d,\"puse\":%d",
       PAL_ItemUseMenuCursor(), (int)PAL_ItemUseMenuRole(),
       PAL_ItemUseMenuActive());
   APP(",\"itm\":%d", g_fItemMenu);
   APP(",\"fp\":\"%llu\"", _bridge_fingerprint());
   APP(",\"inbattle\":%d", gpGlobals->fInBattle ? 1 : 0);
   // auto-battle is on: the game's own AI is playing this fight
   APP(",\"ab\":%d", gpGlobals->fAutoBattle ? 1 : 0);
   APP(",\"srv\":%d,\"clr\":%d,\"press\":%d,\"act\":%d",
       g_nServed, g_nCleared, (int)g_InputState.dwKeyPress, g_iAction);
   APP(",\"ui\":%d,\"dlg\":%d,\"dlgi\":%d,\"wait\":%d",
       g_nUI, (g_nDialog || g_fWaitKey) ? 1 : 0, g_iDialogMsg, g_fWaitKey);
   // ctl: the world loop ran since the last report, so direction keys walk.
   // Anything modal -- menu, shop, battle UI, cutscene -- leaves it 0, which
   // is the only honest way to tell "stuck in a menu" from "standing still".
   APP(",\"ctl\":%d,\"cfm\":%d", g_nWorldFrames > 0 ? 1 : 0, g_fConfirm);
   // Latched BEFORE the counter is cleared: this is the state the agent
   // will see when it decides whether a {"save":N} is legitimate.
   g_fSaveAllowed = _bridge_save_allowed();
   APP(",\"saveok\":%d,\"saved\":%d,\"scriptdepth\":%d,"
       "\"scriptat\":%d,\"autoat\":%d,\"evobj\":%d",
       g_fSaveAllowed, g_nSaveCount, g_nScriptDepth,
       (int)g_iCurrentScriptAddr, (int)g_iAutoScriptAddr,
       (int)g_iCurrentEventObject);
   //
   // USER 12:24 step 4: WORLD-state truth the causal chain needs.
   //   collect : gpGlobals->wCollectValue (0x0033/0x0034 收妖累积值)
   //   learned : rgwMagic[slot][role] -> per-role learned spell ids.
   //             The old "magic.mspells" only exists while the menu is open.
   //   onenter : rgScene[].wScriptOnEnter at THIS frame. 0x006D installs it
   //             mid-story, so the attach-time full dump goes stale. Keeping
   //             these in the state dump avoids a new dump command and leaves
   //             DumpBuilder.parseObjectOverlay() untouched.
   //
   APP(",\"collect\":%d", (int)gpGlobals->wCollectValue);
   APP(",\"learned\":[");
   for (j = 0; j < MAX_PLAYER_ROLES; j++)
   {
      int first = 1;
      APP("%s[", j ? "," : "");
      for (i = 0; i < MAX_PLAYER_MAGICS; i++)
      {
         WORD w = gpGlobals->g.PlayerRoles.rgwMagic[i][j];
         if (w != 0)
         {
            APP("%s%d", first ? "" : ",", (int)w);
            first = 0;
         }
      }
      APP("]");
   }
   APP("]");
   APP(",\"onenter\":[");
   for (i = 0; i < MAX_SCENES; i++)
      APP("%s%d", i ? "," : "",
          (int)gpGlobals->g.rgScene[i].wScriptOnEnter);
   APP("]");
   g_nWorldFrames = 0;
   APP(",\"said\":[");
   for (i = 0; i < g_nSaid; i++)
      APP("%s%d", i ? "," : "", g_rgSaid[i]);
   APP("]");
   g_nSaid = 0;                     // each line is reported exactly once

   // party members
   // M172: "st" is only a 9-bit presence mask. Export the actual
   // remaining-turn counters ("sts", global.h rgPlayerStatus) and the
   // separate poison slots ("ps", rgPoisonStatus/rgObject.poison) so the
   // planner can decide WHICH cure item (0x002B by kind / 0x002C by level)
   // and how urgent it is, instead of guessing from party HP pressure.
   APP(",\"roles\":[");
   for (i = 0; i <= gpGlobals->wMaxPartyMemberIndex; i++)
   {
      {
         int r = (int)gpGlobals->rgParty[i].wPlayerRole;
         int mask = 0;
         for (j = 0; j < kStatusAll; j++)
            if (gpGlobals->rgPlayerStatus[r][j])
               mask |= (1 << j);
         // mh/mm are the engine's LIVE maxima (global.h PLAYERROLES
         // rgwMaxHP/rgwMaxMP). They grow on level-up and are the exact
         // ceiling for every heal/MP potion, so the agent can compute
         // the whole supply plan offline instead of guessing a floor
         // such as lv*15 or probing with failed item uses.
         APP("%s{\"r\":%d,\"hp\":%d,\"mp\":%d,\"mh\":%d,\"mm\":%d,"
             "\"lv\":%d,\"st\":%d,\"eq\":[",
             i ? "," : "",
             r,
             (int)gpGlobals->g.PlayerRoles.rgwHP[r],
             (int)gpGlobals->g.PlayerRoles.rgwMP[r],
             (int)gpGlobals->g.PlayerRoles.rgwMaxHP[r],
             (int)gpGlobals->g.PlayerRoles.rgwMaxMP[r],
             (int)gpGlobals->g.PlayerRoles.rgwLevel[r],
             mask);
         // what each of the 6 body slots holds (0 = bare); a plot that
         // needs a worn item can only test the bag, so the agent has to
         // see this to know what to take off
         for (j = 0; j < MAX_PLAYER_EQUIPMENTS; j++)
            APP("%s%d", j ? "," : "",
                (int)gpGlobals->g.PlayerRoles.rgwEquipment[j][r]);
         APP("],\"sts\":[");
         for (j = 0; j < kStatusAll; j++)
            APP("%s%d", j ? "," : "",
                (int)gpGlobals->rgPlayerStatus[r][j]);
         APP("],\"ps\":[");
         for (j = 0; j < MAX_POISONS; j++)
         {
            int pid = (int)gpGlobals->rgPoisonStatus[j][r].wPoisonID;
            if (pid == 0)
               break;
            APP("%s[%d,%d]", j ? "," : "", pid,
                (int)gpGlobals->g.rgObject[pid].poison.wPoisonLevel);
         }
         APP("]}");
      }
   }
   APP("]");

   // inventory
   APP(",\"inv\":[");
   {
      int first = 1;
      for (i = 0; i < MAX_INVENTORY; i++)
      {
         if (gpGlobals->rgInventory[i].wItem == 0)
            continue;
         // USER「为啥决策会去拿一个没有的道具 / 一定要拿实时的信息」:
         // nAmountInUse 是本回合已锁定要用的数量；itemmenu.c:150/288
         // 用 nAmount <= nAmountInUse 判「不可选」。旧 dump 只给
         // nAmount，Java 看到 x1 就以为还能用，于是反复确认一个
         // 引擎已判不可选的格子。第三个字段让决策用引擎实时真值。
         APP("%s[%d,%d,%d]", first ? "" : ",",
             (int)gpGlobals->rgInventory[i].wItem,
             (int)gpGlobals->rgInventory[i].nAmount,
             (int)gpGlobals->rgInventory[i].nAmountInUse);
         first = 0;
      }
   }
   APP("]");

   // live event objects of the current scene
   APP(",\"objs\":[");
   {
      int first = 1;
      int start = gpGlobals->g.rgScene[gpGlobals->wNumScene - 1].wEventObjectIndex;
      int end = (gpGlobals->wNumScene < MAX_SCENES)
         ? gpGlobals->g.rgScene[gpGlobals->wNumScene].wEventObjectIndex
         : MAX_EVENT_OBJECTS;
      for (i = start; i < end && i < MAX_EVENT_OBJECTS; i++)
      {
         LPEVENTOBJECT p = &gpGlobals->g.lprgEventObject[i];
         APP("%s[%d,%d,%d,%d,%d,%d,%d]", first ? "" : ",",
             i, (int)p->x, (int)p->y, (int)p->sState,
             (int)p->wTriggerScript, (int)p->wAutoScript,
             (int)p->wTriggerMode);
         first = 0;
      }
   }
   APP("]");

   // battle snapshot. ui/menu/sel/cur/plr are the live battle-menu state
   // (BATTLEUISTATE / BATTLEMENUSTATE / selected action icon / cursor /
   // whose turn), so an agent can play the fight with the real keys:
   // ui=1 means the action menu is waiting, ui=2 the enemy cursor is.
   // "hp" is current health; an enemy leaves the list when it falls (id 0).
   if (gpGlobals->fInBattle)
   {
      APP(",\"battle\":{\"result\":%d,\"turn\":%d,"
          "\"ui\":%d,\"menu\":%d,\"sel\":%d,\"cur\":%d,\"plr\":%d,\"enemies\":[",
          (int)g_Battle.BattleResult, (int)g_Battle.wMaxEnemyIndex + 1,
          (int)g_Battle.UI.state, (int)g_Battle.UI.MenuState,
          (int)g_Battle.UI.wSelectedAction,
          (int)g_Battle.UI.iSelectedIndex,
          (int)g_Battle.UI.wCurPlayerIndex);
      // M172: enemy-side truth for the status guard and the steal planner.
      // "si" = nStealItem left (fight.c:5285 refuses to steal when 0),
      // "it" = wStealItem (0 means the loot is CASH, fight.c:5288-5295),
      // "sts" = rgwStatus[kStatusAll] remaining turns (battle.h:74),
      // "ps"  = rgPoisons[] poison ids + levels (battle.h:76).
      for (i = 0; i <= g_Battle.wMaxEnemyIndex; i++)
      {
         APP("%s{\"id\":%d,\"hp\":%d,\"si\":%d,\"it\":%d,\"sts\":[",
             i ? "," : "",
             (int)g_Battle.rgEnemy[i].wObjectID,
             (int)g_Battle.rgEnemy[i].e.wHealth,
             (int)g_Battle.rgEnemy[i].e.nStealItem,
             (int)g_Battle.rgEnemy[i].e.wStealItem);
         for (j = 0; j < kStatusAll; j++)
            APP("%s%d", j ? "," : "",
                (int)g_Battle.rgEnemy[i].rgwStatus[j]);
         APP("],\"ps\":[");
         for (j = 0; j < MAX_POISONS; j++)
         {
            int pid = (int)g_Battle.rgEnemy[i].rgPoisons[j].wPoisonID;
            if (pid == 0)
               break;
            APP("%s[%d,%d]", j ? "," : "", pid,
                (int)gpGlobals->g.rgObject[pid].poison.wPoisonLevel);
         }
         APP("]}");
      }
      // mcur/mnum/mobj = which spell the magic menu is pointing at RIGHT
      // NOW. mobj is its OBJECT id (0 when the menu is closed), so an agent
      // can verify the selection against DATA.MKF before confirming.
      //
      // mspells = the FULL list of spells this role has actually learned
      // (magicmenu.c PAL_MagicSelectionMenuInit only fills rgwMagic[i][role]).
      // USER: combat_policy.json spellPlan is a whole-career table; at lv1
      // the party only knows 296 (气疗术), so asking for 377 (飞龙探云手)
      // sent the agent into the magic menu forever -- right clamps at the
      // last item and the target spell simply does not exist. Export the
      // runtime truth so the planner can check BEFORE entering the menu.
      APP("],\"mspells\":[");
      for (i = 0; i < PAL_MagicMenuCount(); i++)
      {
         APP("%s%d", i ? "," : "", (int)PAL_MagicMenuSpell(i));
      }
      APP("],\"mcur\":%d,\"mnum\":%d,\"mobj\":%d",
          PAL_MagicMenuCursor(), PAL_MagicMenuCount(),
          (int)PAL_MagicMenuSpell(PAL_MagicMenuCursor()));

      // USER「感觉信息不够」：mspells 是【当前打开的法术菜单】内容
      // (magicmenu.c rgMagicItem)，不是【当前决策角色已学的法术】。
      // CLASSIC 全员先决策，plr 换人但菜单未重开时 rgMagicItem 仍是
      // 上一角色的表 —— 真局给灵儿选了逍遥专属气疗术 296，打开
      // 菜单找不到只能 menu 取消，整轮白费。
      // pspells 直接读 PlayerRoles.rgwMagic[][role]，按
      // battle.UI.wCurPlayerIndex 取当前决策角色，是与菜单无关的
      // 引擎真值。Java 候选生成优先用 pspells。
      {
         int plrIdx = (int)g_Battle.UI.wCurPlayerIndex;
         int firstSpell = 1;
         APP(",\"pspells\":[");
         if (plrIdx >= 0 && plrIdx <= gpGlobals->wMaxPartyMemberIndex)
         {
            WORD prole = gpGlobals->rgParty[plrIdx].wPlayerRole;
            for (i = 0; i < MAX_PLAYER_MAGICS; i++)
            {
               WORD w = gpGlobals->g.PlayerRoles.rgwMagic[i][prole];
               if (w == 0) continue;
               APP("%s%d", firstSpell ? "" : ",", (int)w);
               firstSpell = 0;
            }
            APP("],\"psrole\":%d", (int)prole);
         }
         else
         {
            APP("],\"psrole\":-1");
         }
      }
      APP("}");
   }

   // WORLD-phase magic menu (PAL_InGameMagicMenu / play.c:607, opened
   // with kKeyForce). magicmenu.c keeps its cursor in a static, so the
   // same truth is available outside battle. Previously mcur/mnum/mobj
   // were dumped ONLY inside the fInBattle block, leaving the agent
   // blind while using healing spells for post-battle supply in the
   // overworld. Duplicate as a top-level "magic" object instead of
   // moving the battle fields (keeps battle JSON schema unchanged).
   APP(",\"magic\":{\"mcur\":%d,\"mnum\":%d,\"mobj\":%d,\"mopen\":%d,\"mspells\":[",
       PAL_MagicMenuCursor(), PAL_MagicMenuCount(),
       (int)PAL_MagicMenuSpell(PAL_MagicMenuCursor()),
       PAL_MagicMenuIsOpen());
   for (i = 0; i < PAL_MagicMenuCount(); i++)
   {
      APP("%s%d", i ? "," : "", (int)PAL_MagicMenuSpell(i));
   }
   APP("]}");

   APP("}");
#undef APP
   return used;
}

// ----------------------------------------------------------------- actions

typedef struct tagKEYMAP
{
   const char *pszName;
   DWORD       dwKey;
} KEYMAP;

//
// Names follow the player's vocabulary, not SDL's. Note the two that trip
// everyone up: in PAL Enter *and* Space are kKeySearch (confirm / examine)
// while Escape is kKeyMenu (cancel). They are aliased both ways here so an
// action log reads naturally.
//
static const KEYMAP g_rgKeyMap[] = {
   {"-",         kKeyNone},
   {"none",      kKeyNone},
   {"menu",      kKeyMenu},
   {"esc",       kKeyMenu},
   {"cancel",    kKeyMenu},
   {"search",    kKeySearch},
   {"enter",     kKeySearch},
   {"space",     kKeySearch},
   {"confirm",   kKeySearch},
   {"down",      kKeyDown},
   {"left",      kKeyLeft},
   {"up",        kKeyUp},
   {"right",     kKeyRight},
   {"pgup",      kKeyPgUp},
   {"pgdn",      kKeyPgDn},
   {"repeat",    kKeyRepeat},
   {"auto",      kKeyAuto},
   {"defend",    kKeyDefend},
   {"useitem",   kKeyUseItem},
   {"throwitem", kKeyThrowItem},
   {"flee",      kKeyFlee},
   {"status",    kKeyStatus},
   {"force",     kKeyForce},
   {"home",      kKeyHome},
   {"end",       kKeyEnd},
   {NULL, 0}
};

static DWORD
_bridge_key(
   const char *psz
)
{
   int i;
   for (i = 0; g_rgKeyMap[i].pszName != NULL; i++)
   {
      if (strcmp(g_rgKeyMap[i].pszName, psz) == 0)
         return g_rgKeyMap[i].dwKey;
   }
   return kKeyNone;
}

//
// The engine has handled the key we were holding: end that action now.
//
//
// Virtual clock. The engine paces animations, fades and cutscenes with
// SDL_GetTicks(), so with a real clock two runs of the *same* input log
// drift apart whenever the machine is differently busy -- which destroys
// reproducibility and makes offline search results meaningless. In agent
// mode every input pump advances a fixed 16 ms of game time instead.
//
unsigned int
PAL_BridgeTicks(
   void
)
{
   if (!g_fEnabled)
      return SDL_GetTicks();   /* the real one: see the #undef above */
   return g_dwTicks;
}

//
// Where in the UI the engine currently is, set by a handful of one-line
// hooks. Cheap, and it turns "it is stuck" into "it is stuck in X".
//
void
PAL_BridgeAdvance(
   unsigned int ms
)
{
   if (g_fEnabled)
      g_dwTicks += ms;
}

void
PAL_BridgeDelay(
   unsigned int ms
)
{
   if (g_fEnabled)
   {
      g_dwTicks += ms;
      return;
   }
   SDL_Delay(ms);
}

VOID
PAL_BridgeMark(
   INT iUI
)
{
   g_nUI = iUI;
}

VOID
PAL_BridgeDialogIndex(
   INT iIndex
)
{
   g_iDialogMsg = iIndex;
   if (g_nSaid < BRIDGE_MAX_SAID)
      g_rgSaid[g_nSaid++] = iIndex;
}

VOID
PAL_BridgeWaitKey(
   INT fWaiting
)
{
   g_fWaitKey = fWaiting ? 1 : 0;
}

VOID
PAL_BridgeDialogBegin(
   VOID
)
{
   g_nDialog++;
}

VOID
PAL_BridgeDialogEnd(
   VOID
)
{
   g_nDialog = 0;
   //
   // Forget the line, or the agent keeps reading the previous speaker's words
   // after the dialog has closed and reports a conversation that never
   // happened.
   //
   g_iDialogMsg = -1;
}

INT
PAL_BridgeDialog(
   VOID
)
{
   return g_nDialog;
}

VOID
PAL_BridgeConsumed(
   VOID
)
{
   if (!g_fEnabled)
      return;
   if (g_iAction < g_nActions && g_rgActions[g_iAction].dwKey != kKeyNone)
   {
      //
      // Only a real keypress can be "consumed". The engine calls
      // PAL_ClearKeyState() routinely, and retiring an idle action there
      // meant an "idle N frames" order ended after a single pump.
      //
      g_rgActions[g_iAction].nFrames = 0;

      //
      // A confirm/cancel key is a single press: once the engine has taken
      // it, move past it NOW. Leaving it for the pump to retire serves it
      // one more time first -- and inside PAL_DialogWaitForKey() that extra
      // press closes the dialog the instant it opens, so every line was gone
      // before the agent could see it. Direction keys are different: walking
      // clears the key state every frame while the key is meant to stay held.
      //
      if (g_rgActions[g_iAction].dwKey & (kKeySearch | kKeyMenu))
         g_iAction++;
   }
   g_nCleared++;
}

//
// Parse {"a":[{...},{...}]} into the action queue.
//
static void
_bridge_parse(
   const char *psz
)
{
   const char *p;
   g_nActions = 0;
   g_iAction = 0;

   if (strstr(psz, "\"quit\"") != NULL)
   {
      g_fQuitRequested = 1;
      return;
   }

   if (strstr(psz, "\"dump\"") != NULL)
   {
      // {"dump":"objs"} = lightweight refresh: only the live event-object
      // array (positions/state/trigger/auto script), which is the ONLY part
      // that剧情 rewrite mutates. The BFS must not keep using a stale
      // trigger script from the attach-time full dump for OTHER scenes while
      // the current scene uses fresh state.objs — that static/realtime split
      // made one (scene,obj) look like two different goals. Full dump (header
      // + scenes + objs + script + maps) stays the default.
      if (strstr(psz, "\"objs\"") != NULL)
         _bridge_dump_objs_only();
      else
         _bridge_dump();
      return;
   }

   //
   // {"save":N} / {"load":N}, optionally together with "a". The same calls
   // the in-game system menu makes (uigame.c "Save game" / "Load game"), so
   // a save is the game's own N.rpg file. The agent uses slots above 10:
   // 1..5 are the player's, and must never be overwritten.
   //
   {
      int iSave = _json_int(psz, "save", 0);
      int iLoad = _json_int(psz, "load", 0);
      if (iSave > 0 && iSave < 1000 && gpGlobals != NULL)
      {
         // A normal player can ONLY save from the system menu, which is
         // reachable exclusively at the tail of PAL_StartFrame(). Refuse
         // saves taken in the middle of a plot script / battle / dialog /
         // modal UI: SAVEDGAME does not store the script program counter,
         // so reloading such a file can never resume the half-run script.
         // "load" is intentionally unrestricted (it is the recovery path).
         // g_fSaveAllowed was latched in _bridge_state() just before
         // g_nWorldFrames was cleared; recomputing here would always be 0.
         if (g_fSaveAllowed)
         {
            PAL_SaveGame(iSave, 1);
            g_nSaveCount++;
         }
         else
            fprintf(stderr,
               "[BRIDGE] save slot %d REFUSED (not in a saveable state)\n",
               iSave);
      }
      if (iLoad > 0 && iLoad < 1000 && gpGlobals != NULL)
      {
         PAL_ReloadInNextTick(iLoad);
         // Release EVERY synchronous wait loop in this pre-battle
         // script (48 dialog lines), not just the first one.
         g_nBridgeRecoveryBudget = BRIDGE_RECOVERY_BUDGET;
         // USER「持续迭代目标通关」: a pre-battle script can contain
         // dozens of dialog lines, each with its OWN WaitForKey loop.
         // A one-shot flag frees only the first one, so the engine
         // never returns to the main loop and PAL_LoadResources()
         // never consumes the reload flags (real run: 16 reload
         // batches, still inbattle=true, result=1001, frozen frame).
         // {"load":N,"abort":1} explicitly ends the battle so
         // PAL_BattleMain()'s "BattleResult != OnGoing" check breaks,
         // control returns to game.c, and the load takes effect.
         // Recovery-only: never used by normal play / plain load.
         if (_json_int(psz, "abort", 0))
         {
            g_Battle.BattleResult = kBattleResultTerminated;
         }
      }
   }

   //
   // {"auto":1}: hand the current/next battle to the game's own auto-battle
   // AI -- the same flag the story scripts set for fights meant to play
   // themselves (script.c 0x0007 clears it when the battle ends). The
   // engine then picks magic/attacks/targets on its own and the battle menu
   // stops taking keys, so the agent needs no fighting skill of its own.
   //
   if (_json_int(psz, "auto", 0) && gpGlobals != NULL)
      gpGlobals->fAutoBattle = TRUE;

   p = strstr(psz, "\"a\"");
   if (p == NULL)
      return;

   p = strchr(p, '[');
   if (p == NULL)
      return;

   while (*p && g_nActions < BRIDGE_MAX_ACTION)
   {
      const char *pszObj = strchr(p, '{');
      if (pszObj == NULL)
         break;
      p = pszObj;

      char szKey[32] = "-";
      int nFrames = 4;
      const char *pszK = strstr(pszObj, "\"k\"");
      if (pszK != NULL)
      {
         pszK = strchr(pszK + 3, '"');
         if (pszK != NULL)
         {
            pszK++;
            int i = 0;
            while (*pszK && *pszK != '"' && i < (int)sizeof(szKey) - 1)
               szKey[i++] = *pszK++;
            szKey[i] = '\0';
         }
      }
      const char *pszH = strstr(pszObj, "\"h\"");
      if (pszH != NULL)
      {
         pszH = strchr(pszH + 3, ':');
         if (pszH != NULL)
            nFrames = atoi(pszH + 1);
      }
      if (nFrames < 1)
         nFrames = 1;

      g_rgActions[g_nActions].dwKey = _bridge_key(szKey);
      g_rgActions[g_nActions].nFrames = nFrames;
      g_nActions++;

      p = strchr(pszObj, '}');
      if (p == NULL)
         break;
      p++;
      if (*p == ',')
         p++;
      if (*p == ']')
         break;
   }
}

// ------------------------------------------------------------ knowledge dump
//
// The agent needs the game's static data -- every scene, every event object,
// every script instruction and every map's collision grid -- to plan. Parsing
// the MKF files in Python means reimplementing YJ_1 decompression, and the
// drift there silently corrupted maps before. The engine already has all of it
// parsed into C structs, so just serialise those: one source of truth, and it
// cannot disagree with what the engine actually does.
//
// Output is newline-delimited JSON so a big dump can be streamed in chunks:
//   {"dump":1}                     header
//   {"n":{...}}                    counts
//   {"scenes":[[map,onEnter,onTeleport,objIndex],...]}
//   {"objs":[[...],[...]]}         chunks of event objects
//   {"script":"<base64>"}          raw script entries, 4 x uint16 each
//   {"objects":"<base64>"}         raw OBJECT union records
//   {"map":N,"n":M,"b":"<base64>"} blocked-tile bitmap for map N
//   {"eod":1}                      end of dump
//
static const char g_szB64[] =
   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t       g_nDumpLen = 0;

static void
_bridge_b64(
   const unsigned char *p,
   size_t               n,
   char                *pszOut
)
/*++
  Purpose:

    Base64 with '=' padding, so the Python side can use the stock decoder.

--*/
{
   size_t i = 0, o = 0;
   int    last = 0;

   while (i < n)
   {
      unsigned long v = 0;
      int           k = 0;

      while (k < 3 && i < n)
      {
         v = (v << 8) | p[i++];
         k++;
      }
      last = k;                 // how many real bytes went into this group
      while (k < 3)
      {
         v <<= 8;
         k++;
      }

      //
      // A short group emits only last+1 characters and is then padded to
      // four. Writing all four and padding afterwards is off by one -- a
      // 2048-byte buffer came back as 2049, and a padded group longer than
      // four characters is not valid base64 at all.
      //
      pszOut[o++] = g_szB64[(v >> 18) & 63];
      pszOut[o++] = g_szB64[(v >> 12) & 63];
      if (last >= 2)
         pszOut[o++] = g_szB64[(v >> 6) & 63];
      if (last >= 3)
         pszOut[o++] = g_szB64[v & 63];
      while (o & 3)
         pszOut[o++] = '=';
      last = 0;                 // only the final group can be short
   }

   pszOut[o] = '\0';
}

static void
_bridge_dump_line(
   const char *psz
)
/*++
  Purpose:

    Append one line to the dump buffer, flushing when it fills.

--*/
{
   size_t n = strlen(psz);

   if (g_nDumpLen + n + 2 > BRIDGE_BUFSZ - 16)
   {
      g_szOut[g_nDumpLen] = '\0';
      _bridge_send(g_szOut);
      g_nDumpLen = 0;
   }

   memcpy(g_szOut + g_nDumpLen, psz, n);
   g_nDumpLen += n;
   g_szOut[g_nDumpLen++] = '\n';
}

static void
_bridge_dump_raw(
   const char *pszHead,
   const char *pszBody,
   const char *pszTail
)
/*++
  Purpose:

    Send one field whose value is bigger than the output buffer -- the script
    table is hundreds of kilobytes. snprintf() would happily truncate it and
    still report the full length, so the value is streamed in chunks instead.

--*/
{
   size_t nb = strlen(pszBody), off = 0;
   char   sz[8192];

   if (g_nDumpLen > 0)
   {
      g_szOut[g_nDumpLen] = '\0';
      _bridge_send(g_szOut);
      g_nDumpLen = 0;
   }

   _bridge_send(pszHead);

   while (off < nb)
   {
      size_t n = nb - off;
      if (n > sizeof(sz) - 1)
         n = sizeof(sz) - 1;
      memcpy(sz, pszBody + off, n);
      sz[n] = '\0';
      _bridge_send(sz);
      off += n;
   }

   _bridge_send(pszTail);
}

static void
_bridge_dump_flush(
   VOID
)
{
   if (g_nDumpLen > 0)
   {
      g_szOut[g_nDumpLen] = '\0';
      _bridge_send(g_szOut);
      g_nDumpLen = 0;
   }
}

//
// Emit the LIVE event-object array as {"objs":[...]} NDJSON chunks.
// These fields come straight from gpGlobals->g.lprgEventObject (the same
// array the engine mutates on 0x0025 rewrite_trigger / 0x0049 set_state),
// so they are the current truth for EVERY scene, not an attach-time snapshot.
//
static void
_bridge_dump_objs(
   VOID
)
{
   int  i, j, chunk = 0;
   for (i = 0; i < gpGlobals->g.nEventObject; )
   {
      char  *p;
      size_t room;
      int    w;

      if (g_nDumpLen > BRIDGE_BUFSZ / 2)
         _bridge_dump_flush();
      p = g_szOut + g_nDumpLen;
      room = BRIDGE_BUFSZ - g_nDumpLen - 64;

      w = snprintf(p, room, "{\"objs\":[");
      for (j = 0; j < 256 && i < gpGlobals->g.nEventObject; j++, i++)
      {
         LPEVENTOBJECT o = &gpGlobals->g.lprgEventObject[i];
         char          item[96];
         int           m;

         m = snprintf(item, sizeof(item), "%s[%d,%d,%d,%d,%d,%d,%d]",
                      j ? "," : "",
                      (int)o->x, (int)o->y, (int)o->sState,
                      (int)o->wTriggerMode, (int)o->wTriggerScript,
                      (int)o->wAutoScript, (int)o->wSpriteNum);
         if (w + m + 2 > (int)room)
            break;
         memcpy(p + w, item, m);
         w += m;
      }
      w += snprintf(p + w, room - w, "]}");
      g_nDumpLen += w;
      g_szOut[g_nDumpLen++] = '\n';
      if (g_nDumpLen > BRIDGE_BUFSZ / 2)
      {
         g_szOut[g_nDumpLen] = '\0';
         _bridge_send(g_szOut);
         g_nDumpLen = 0;
      }
      chunk++;
   }
   if (chunk == 0)
      _bridge_dump_line("{\"objs\":[]}");
}

//
// Lightweight dump: header + live objs + eod only. No scenes/script/maps
// resend. The agent calls this every round to refresh its BFS object layer
// so all scenes are judged from ONE fresh snapshot (no static/realtime split).
//
static void
_bridge_dump_objs_only(
   VOID
)
{
   g_nDumpLen = 0;
   _bridge_dump_line("{\"dump\":\"objs\"}");
   _bridge_dump_objs();
   _bridge_dump_line("{\"eod\":1}");
   _bridge_dump_flush();
}

static void
_bridge_dump(
   VOID
)
{
   int        i, j, n;
   char       sz[256];
   char      *pszBuf;
   FILE      *fpMAP, *fpGOP;

   g_nDumpLen = 0;
   _bridge_dump_line("{\"dump\":1}");

   snprintf(sz, sizeof(sz),
            "{\"n\":{\"scene\":%d,\"obj\":%d,\"script\":%d,\"object\":%d}}",
            MAX_SCENES, (int)gpGlobals->g.nEventObject,
            (int)gpGlobals->g.nScriptEntry, MAX_OBJECTS);
   _bridge_dump_line(sz);

   //
   // Scenes: map number and where the scene's event objects start.
   //
   {
      char *p;
      size_t room;
      int    w = 0;

      if (g_nDumpLen > BRIDGE_BUFSZ / 2)
         _bridge_dump_flush();
      p = g_szOut + g_nDumpLen;
      room = BRIDGE_BUFSZ - g_nDumpLen - 64;

      w = snprintf(p, room, "{\"scenes\":[");
      for (i = 0; i < MAX_SCENES; i++)
      {
         char item[64];
         int  m;

         m = snprintf(item, sizeof(item), "%s[%d,%d,%d,%d]",
                      i ? "," : "",
                      (int)gpGlobals->g.rgScene[i].wMapNum,
                      (int)gpGlobals->g.rgScene[i].wScriptOnEnter,
                      (int)gpGlobals->g.rgScene[i].wScriptOnTeleport,
                      (int)gpGlobals->g.rgScene[i].wEventObjectIndex);
         if (w + m + 2 > (int)room)
            break;
         memcpy(p + w, item, m);
         w += m;
      }
      w += snprintf(p + w, room - w, "]}");
      g_nDumpLen += w;
      g_szOut[g_nDumpLen++] = '\n';
   }

   //
   // Event objects (live struct snapshot). Shared with the lightweight refresh.
   //
   _bridge_dump_objs();

   //
   // Script table, verbatim: 4 x uint16 per entry. Interpreting opcodes is the
   // knowledge base's job; the engine just hands over the bytes.
   //
   if (gpGlobals->g.lprgScriptEntry != NULL && gpGlobals->g.nScriptEntry > 0)
   {
      n = (int)gpGlobals->g.nScriptEntry * 8;
      pszBuf = (char *)malloc((size_t)n / 3 * 4 + 16);
      if (pszBuf != NULL)
      {
         _bridge_b64((const unsigned char *)gpGlobals->g.lprgScriptEntry,
                     (size_t)n, pszBuf);
         _bridge_dump_raw("{\"script\":\"", pszBuf, "\"}\n");
         free(pszBuf);
      }
   }

   //
   // OBJECT union (players, items, magics, enemies) -- same reasoning.
   //
   {
      n = MAX_OBJECTS * sizeof(OBJECT);
      pszBuf = (char *)malloc((size_t)n / 3 * 4 + 16);
      if (pszBuf != NULL)
      {
         _bridge_b64((const unsigned char *)gpGlobals->g.rgObject,
                     (size_t)n, pszBuf);
         {
            char szHead[64];
            snprintf(szHead, sizeof(szHead),
                     "{\"objectsz\":%d,\"objects\":\"", (int)sizeof(OBJECT));
            _bridge_dump_raw(szHead, pszBuf, "\"}\n");
         }
         free(pszBuf);
      }
   }

   //
   // Items, read through the OBJECT union rather than by guessing the record
   // stride: the union is sizeof(OBJECT) wide (its largest member rules) while
   // an item record is only six words, so slicing it by stride silently reads
   // another member's field as the flags.
   //
   {
      if (g_nDumpLen > BRIDGE_BUFSZ / 2)
         _bridge_dump_flush();
      {
         char *p = g_szOut + g_nDumpLen;
         size_t room = BRIDGE_BUFSZ - g_nDumpLen - 64;
         int    w;

         w = snprintf(p, room, "{\"items\":[");
         for (i = 0; i < MAX_OBJECTS; i++)
         {
            char item[96];
            int  m;

            m = snprintf(item, sizeof(item), "%s[%d,%d,%d,%d,%d,%d]",
                         i ? "," : "",
                         (int)gpGlobals->g.rgObject[i].item.wBitmap,
                         (int)gpGlobals->g.rgObject[i].item.wPrice,
                         (int)gpGlobals->g.rgObject[i].item.wScriptOnUse,
                         (int)gpGlobals->g.rgObject[i].item.wScriptOnEquip,
                         (int)gpGlobals->g.rgObject[i].item.wScriptOnThrow,
                         (int)gpGlobals->g.rgObject[i].item.wFlags);
            if (w + m + 2 > (int)room)
               break;
            memcpy(p + w, item, m);
            w += m;
         }
         w += snprintf(p + w, room - w, "]}");
         g_nDumpLen += w;
         g_szOut[g_nDumpLen++] = '\n';
      }
   }

   //
   // Maps: one blocked bit per tile, (y * 64 + x) * 2 + h. The engine loads
   // each map through its own loader, so the grid is exactly what
   // PAL_MapTileIsBlocked() would answer.
   //
   fpMAP = fopen("map.mkf", "rb");
   fpGOP = fopen("gop.mkf", "rb");

   if (fpMAP != NULL && fpGOP != NULL)
   {
      int nMaps = PAL_MKFGetChunkCount(fpMAP);

      for (i = 1; i < nMaps; i++)
      {
         LPPALMAP lpMap = PAL_LoadMap(i, fpMAP, fpGOP);

         if (lpMap == NULL)
            continue;

         {
            unsigned char *pBits = (unsigned char *)malloc(2048);
            if (pBits != NULL)
            {
               memset(pBits, 0, 2048);
               for (j = 0; j < 128 * 64 * 2; j++)
               {
                  int y = j / 128;
                  int x = (j / 2) % 64;
                  int h = j & 1;
                  if (PAL_MapTileIsBlocked((BYTE)x, (BYTE)y, (BYTE)h, lpMap))
                     pBits[j >> 3] |= (unsigned char)(1 << (j & 7));
               }
               pszBuf = (char *)malloc(2048 / 3 * 4 + 16);
               if (pszBuf != NULL)
               {
                  char szHead[64];
                  _bridge_b64(pBits, 2048, pszBuf);
                  snprintf(szHead, sizeof(szHead), "{\"map\":%d,\"b\":\"", i);
                  _bridge_dump_raw(szHead, pszBuf, "\"}\n");
                  free(pszBuf);
               }
               free(pBits);
            }
         }

         PAL_FreeMap(lpMap);

         if (g_nDumpLen > BRIDGE_BUFSZ / 2)
         {
            g_szOut[g_nDumpLen] = '\0';
            _bridge_send(g_szOut);
            g_nDumpLen = 0;
         }
      }
   }

   if (fpMAP != NULL)
      fclose(fpMAP);
   if (fpGOP != NULL)
      fclose(fpGOP);

   _bridge_dump_line("{\"eod\":1}");
   _bridge_dump_flush();
}

// -------------------------------------------------------------------- init

// ---------------------------------------------------------------------
// HUMAN PLAY RECORDER (USER 22:48). Opt-in via PAL_HUMAN_LOG=<path>.
// Works in the NORMAL windowed game: PAL_ProcessEvent() always funnels
// through PAL_BridgePump(), which is where we sample the engine's own
// globals. Nothing is injected into the input path -- the human keeps
// full control. Only CHANGES are written, so a 60 fps session stays a
// compact, replayable JSONL: keys, scene, party position, facing, battle.
// ---------------------------------------------------------------------
#include <sys/time.h>

static FILE      *g_humanLog = NULL;
static DWORD      g_humanFrame = 0;
static DWORD      g_hLastKey = 0xFFFFFFFFu;
static WORD       g_hLastScene = 0xFFFF;
static SHORT      g_hLastX = -1, g_hLastY = -1;
static WORD       g_hLastDir = 0xFFFF;
static BOOL       g_hLastBattle = FALSE;
static INT        g_hLastDlg = -1;
static INT        g_hLastMsg = -2;

static unsigned long long
_human_ms(
   VOID
)
{
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return (unsigned long long)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

static VOID
_human_keys(
   DWORD    key,
   char    *buf,
   size_t   n
)
{
   static const struct { DWORD bit; const char *name; } names[] = {
      { kKeyMenu, "menu" },     { kKeySearch, "search" },
      { kKeyUseItem, "item" },  { kKeyUp, "up" },
      { kKeyDown, "down" },     { kKeyLeft, "left" },
      { kKeyRight, "right" },   { kKeyFlee, "flee" },
   };
   size_t used = 0;
   unsigned i;
   buf[0] = '\0';
   for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
   {
      if (key & names[i].bit)
      {
         int w = snprintf(buf + used, n - used, "%s%s",
                          used ? "|" : "", names[i].name);
         if (w < 0 || (size_t)w >= n - used)
            break;
         used += (size_t)w;
      }
   }
}

VOID
PAL_HumanLogInit(
   VOID
)
{
   const char *p = getenv("PAL_HUMAN_LOG");
   if (p == NULL || !*p)
      return;
   g_humanLog = fopen(p, "a");
   if (g_humanLog == NULL)
      return;
   setvbuf(g_humanLog, NULL, _IOLBF, 0);
   fprintf(stderr, "HUMANLOG -> %s\n", p);
}

VOID
PAL_HumanLogFrame(
   VOID
)
{
   char   keys[160];
   WORD   scene, dir;
   SHORT  x, y;
   DWORD  key;
   BOOL   battle;
   INT    dlg, msg;

   if (g_humanLog == NULL || gpGlobals == NULL)
      return;
   g_humanFrame++;
   scene  = gpGlobals->wNumScene;
   /* rgParty[0].x/y are NOT map coordinates (they stayed 160,112 for a
      whole human session). The real party position is viewport +
      partyoffset, exactly as _bridge_state() reports it at line 332.
      Without this, a recorded play-through cannot tell WHERE a chest or
      a door was triggered. */
   x      = PAL_X(gpGlobals->viewport) + PAL_X(gpGlobals->partyoffset);
   y      = PAL_Y(gpGlobals->viewport) + PAL_Y(gpGlobals->partyoffset);
   dir    = gpGlobals->wPartyDirection;
   battle = gpGlobals->fInBattle;
   key    = g_InputState.dwKeyPress;
   dlg    = g_nDialog;
   msg    = g_iDialogMsg;

   if (key == g_hLastKey && scene == g_hLastScene && x == g_hLastX &&
       y == g_hLastY && dir == g_hLastDir && battle == g_hLastBattle &&
       dlg == g_hLastDlg && msg == g_hLastMsg)
      return;

   _human_keys(key, keys, sizeof(keys));
   fprintf(g_humanLog,
      "{\"ms\":%llu,\"frame\":%lu,\"scene\":%u,\"x\":%d,\"y\":%d,"
      "\"dir\":%u,\"battle\":%d,\"dlg\":%d,\"msg\":%d,"
      "\"key\":%lu,\"keys\":\"%s\"}\n",
      _human_ms(), (unsigned long)g_humanFrame, (unsigned)scene,
      (int)x, (int)y, (unsigned)dir, battle ? 1 : 0,
      (int)dlg, (int)msg, (unsigned long)key, keys);

   g_hLastKey = key;
   g_hLastScene = scene;
   g_hLastX = x;
   g_hLastY = y;
   g_hLastDir = dir;
   g_hLastBattle = battle;
   g_hLastDlg = dlg;
   g_hLastMsg = msg;
}

//
// USER 10:11: the GUI owns the socket and an agent CONNECTS TO the game.
// The hello/ack handshake used to be inline in PAL_BridgeInit (once, at
// startup). It is now reusable so every fresh agent attachment re-handshakes
// against the CURRENT in-memory state, with no game restart.
//
static INT
_bridge_handshake(
   INT fExitOnFail
)
{
   char szHello[4096];
   char *pszLine;
   int seed, fast;

   snprintf(szHello, sizeof(szHello),
            "{\"t\":\"hello\",\"ver\":%d,\"win95\":%d,\"files\":[",
            BRIDGE_VERSION, gConfig.fIsWIN95 ? 1 : 0);
   {
      static const char *rgsz[] = {"sss.mkf", "map.mkf", "gop.mkf",
                                   "fbp.mkf", "m.msg",   "word.dat",
                                   "ball.mkf", "data.mkf"};
      int i;
      for (i = 0; i < (int)(sizeof(rgsz) / sizeof(rgsz[0])); i++)
      {
         FILE *fp = fopen(rgsz[i], "rb");
         long n = -1;
         if (fp != NULL)
         {
            fseek(fp, 0, SEEK_END);
            n = ftell(fp);
            fclose(fp);
         }
         snprintf(szHello + strlen(szHello),
                  sizeof(szHello) - strlen(szHello),
                  "%s{\"n\":\"%s\",\"s\":%ld}", i ? "," : "", rgsz[i], n);
      }
   }
   strncat(szHello, "]}\n", sizeof(szHello) - strlen(szHello) - 1);

   if (!_bridge_send(szHello))
   {
      fprintf(stderr, "[BRIDGE] hello send FAILED fd=%d\n", (int)g_sock);
      return 0;
   }
   fprintf(stderr, "[BRIDGE] hello sent fd=%d\n", (int)g_sock);

   pszLine = _bridge_recv_line();
   if (pszLine == NULL)
   {
      if (fExitOnFail)
         exit(0);              // legacy connect mode: agent died at hello
      return 0;                // listen mode: just drop this attachment
   }
   seed = _json_int(pszLine, "seed", 1);
   fast = _json_int(pszLine, "fast", g_fFast);
   g_fFast = fast ? 1 : 0;
   if (seed > 0)
      PAL_BridgeSetSeed(seed);
   return 1;
}

//
// Listen mode: bind a socket and let the game run standalone (g_fEnabled=0).
// Agents attach later via _bridge_try_accept(). Non-blocking so a GUI with
// no agent attached still animates and accepts human keyboard input.
//
static INT
_bridge_start_listen(
   int port
)
{
   struct sockaddr_in addr;
   int reuse = 1;

   g_listenSock = socket(AF_INET, SOCK_STREAM, 0);
   if (g_listenSock == PAL_INVALID_SOCK)
      return 0;
   setsockopt(g_listenSock, SOL_SOCKET, SO_REUSEADDR,
              (const char *)&reuse, sizeof(reuse));
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons((unsigned short)port);
   addr.sin_addr.s_addr = htonl(INADDR_ANY);
   if (bind(g_listenSock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
       listen(g_listenSock, 1) != 0)
   {
      PAL_CLOSE(g_listenSock);
      g_listenSock = PAL_INVALID_SOCK;
      return 0;
   }
#if !defined(_WIN32)
   fcntl(g_listenSock, F_SETFL,
         fcntl(g_listenSock, F_GETFL, 0) | O_NONBLOCK);
#endif
   g_fListen = 1;
   g_fEnabled = 0;            // standalone until an agent attaches
   fprintf(stderr, "BRIDGE listen port=%d (GUI runs standalone; "
                   "agent may attach at any state)\n", port);
   return 1;
}

//
// Called every pump while no agent is attached. A successful accept takes
// over the CURRENT state: re-handshake, reset the input queue, and start
// the strict one-report / one-batch alternation from wherever the game is.
//
static VOID
_bridge_try_accept(
   VOID
)
{
   PAL_SOCKET c = accept(g_listenSock, NULL, NULL);
   if (c == PAL_INVALID_SOCK)
      return;                 // no agent yet: standalone frame
   fprintf(stderr, "[BRIDGE] accept ok fd=%d enabled=%d listen=%d\n",
           (int)c, g_fEnabled, g_fListen);
#if !defined(_WIN32)
   fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) & ~O_NONBLOCK);
   //
   // M226: a stale/half-closed connection in the accept backlog used to
   // block _bridge_handshake() forever in recv(), freezing the main loop
   // so the REAL next agent was never accepted. Bound the handshake read.
   //
   {
      struct timeval tv;
      tv.tv_sec = 2;
      tv.tv_usec = 0;
      setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const void *)&tv, sizeof(tv));
   }
#endif
   g_sock = c;
   g_nInLen = 0;
   g_nActions = 0;
   g_iAction = 0;
   g_fQuitRequested = 0;
   g_nCleared = 0;
   g_nServed = 0;
   g_nWorldFrames = 0;
   if (!_bridge_handshake(0))
   {
      fprintf(stderr, "[BRIDGE] handshake FAILED fd=%d\n", (int)g_sock);
      PAL_CLOSE(g_sock);
      g_sock = PAL_INVALID_SOCK;
      return;
   }
   g_fEnabled = 1;
   fprintf(stderr, "[BRIDGE] handshake OK fd=%d\n", (int)g_sock);
   //
   // M226: the standalone frames computed dwTime (game.c) with the REAL
   // SDL_GetTicks(). Once g_fEnabled=1, PAL_BridgeTicks() switches to the
   // virtual g_dwTicks, which still starts at 0. Any tick-based wait
   // (PAL_DelayUntil under cinematics/non-fast, PAL_WaitForKey*, battle
   // animations) then chases a huge deadline one pump at a time: the main
   // loop never reaches PAL_ClearKeyState/PAL_StartFrame, so clr=0, ctl=0,
   // keys are recorded but the party never moves -- looks like a dead GUI.
   // bridge.c has #undef'd SDL_GetTicks above, so this is the real clock.
   // Align the virtual clock to it at the attach boundary.
   //
   _bridge_clock_domain_reset();
   fprintf(stderr, "BRIDGE agent attached; taking over current state"
           " (clock aligned %u)\n", g_dwTicks);
}

VOID
PAL_BridgeInit(
   VOID
)
{
   const char *psz;
   struct sockaddr_in addr;
   int port = 4567;
   const char *pszHost = "127.0.0.1";

   // The human recorder is independent of the agent bridge: normal GUI
   // play has PAL_AGENT unset, so it must start here before that check.
   PAL_HumanLogInit();

   psz = getenv("PAL_AGENT");
   if (psz == NULL || atoi(psz) != 1)
      return;

   psz = getenv("PAL_AGENT_PORT");
   if (psz != NULL)
      port = atoi(psz);
   psz = getenv("PAL_AGENT_HOST");
   if (psz != NULL && *psz)
      pszHost = psz;

   g_fFast = 1;
   psz = getenv("PAL_AGENT_FAST");
   if (psz != NULL)
      g_fFast = atoi(psz);

   g_fNoRender = 1;
   psz = getenv("PAL_AGENT_RENDER");
   if (psz != NULL)
      g_fNoRender = !atoi(psz);

   // USER 21:29/21:35: a WATCHABLE run keeps fast=1 (the one-report /
   // one-batch protocol and all skipped waits stay exactly as they are)
   // but paces each engine frame in REAL time. PAL_AGENT_FRAME_MS=16 is
   // ~60 fps. Default 0 = unchanged headless speed.
   psz = getenv("PAL_AGENT_FRAME_MS");
   if (psz != NULL)
      g_nFrameMs = atoi(psz);
   // Sound is a separate opt-in: audio.c/midi.c otherwise bail out on
   // PAL_BridgeFast(), and main.c only opens the device when !fast.
   psz = getenv("PAL_AGENT_AUDIO");
   if (psz != NULL)
      g_fAudio = atoi(psz);
   // USER 22:03: headless TRAINING (fast=1) may skip movies/fades, but
   // the final THEATRICAL run must play the game NORMALLY. This opt-in
   // re-enables RNG movies, AVI cutscenes, palette fades and real waits
   // while the one-report/one-batch protocol stays intact.
   psz = getenv("PAL_AGENT_CINEMATICS");
   if (psz != NULL)
      g_fCinematics = atoi(psz);

   // USER 22:25 "你每次都是重新开的局！！！我看着呢！": --resume used to
   // press through PAL_OpeningMenu() and the WHOLE opening cutscene first
   // and only THEN load slot 11, so every window restart showed a brand
   // new game. A boot slot makes the engine initialise from the save
   // directly, skipping the trademark/splash/opening entirely.
   psz = getenv("PAL_AGENT_BOOT_SLOT");
   if (psz != NULL)
      g_iBootSlot = atoi(psz);

   // USER 22:15: never again GUESS whether the presentation switches
   // reached the engine. This one line is captured by PAL_ENGINE_STDERR
   // and proves the actual runtime values at process start.
   fprintf(stderr,
      "BRIDGE flags: fast=%d render=%s cinematics=%d audio=%d frame_ms=%d boot_slot=%d\n",
      g_fFast, g_fNoRender ? "off" : "on", g_fCinematics, g_fAudio,
      g_nFrameMs, g_iBootSlot);

   // USER 10:11 "游戏启动后监听 socket，agent 去连游戏，任意状态可接管":
   // PAL_AGENT_LISTEN=1 makes the GUI own the socket. It runs standalone
   // (render=on, human-playable) until an agent connects and takes over
   // whatever state the game is currently in. Default 0 keeps the legacy
   // connect-mode used by headless self-iteration, byte-for-byte unchanged.
   psz = getenv("PAL_AGENT_LISTEN");
   if (psz != NULL && atoi(psz) == 1)
   {
      _bridge_start_listen(port);
      return;
   }

   g_sock = socket(AF_INET, SOCK_STREAM, 0);
   if (g_sock == PAL_INVALID_SOCK)
      return;

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = htons((unsigned short)port);
   addr.sin_addr.s_addr = inet_addr(pszHost);
   if (addr.sin_addr.s_addr == INADDR_NONE)
   {
      struct hostent *he = gethostbyname(pszHost);
      if (he == NULL)
      {
         PAL_CLOSE(g_sock);
         g_sock = PAL_INVALID_SOCK;
         return;
      }
      memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
   }

   if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
   {
      PAL_CLOSE(g_sock);
      g_sock = PAL_INVALID_SOCK;
      return;
   }

   g_fEnabled = 1;
   _bridge_handshake(1);
}
INT
PAL_BridgeEnabled(
   VOID
)
{
   return g_fEnabled;
}

INT
PAL_BridgeFast(
   VOID
)
{
   return g_fEnabled && g_fFast;
}

INT
PAL_BridgeNoRender(
   VOID
)
{
   return g_fEnabled && g_fFast && g_fNoRender;
}
INT
PAL_BridgeAudio(
   VOID
)
{
   return g_fEnabled && g_fAudio;
}
INT
PAL_BridgeCinematics(
   VOID
)
{
   return g_fEnabled && g_fCinematics;
}

INT
PAL_BridgeBootSlot(
   VOID
)
{
   // USER 11:11 定稿：不设 boot slot（默认 0）时从新游戏开始，存档位
   // 只是 agent 的存/读档槽；显式设置 PAL_AGENT_BOOT_SLOT 时，游戏
   // 必须直接从该存档初始化（game.c:57 / main.c:577 都读这里）。
   //
   // 2026-11-23 修复：旧实现返回 g_fEnabled ? g_iBootSlot : 0，而
   // LISTEN 模式启动时 g_fEnabled=0（bridge.c:1649 "standalone until
   // an agent attaches"），于是 gui.sh 的 PAL_AGENT_BOOT_SLOT=90 只是
   // 打印在 flags 里、从不生效，每次都变成「新游戏 + agent 按 enter」。
   // 用户要求「从 90 存档开始」时无法坐实起点。启动期 boot slot 与
   // agent 是否 attach 无关，直接返回；默认 0 的旧行为完全不变。
   return g_iBootSlot;
}

// -------------------------------------------------------------------- pump

//
// Hold a key the way PAL_UpdateKeyboardState() would.
//
// Walking reads g_InputState.dir (scene.c:805), not dwKeyPress: the key bits
// alone move nothing, which is why the party stood still no matter what the
// agent sent. Direction and key have to be set together, exactly as the touch
// and mouse paths in input.c do.
//
static void
_bridge_setkey(
   DWORD dwKey
)
{
   int dir = kDirUnknown;

   if (dwKey & kKeyUp)
      dir = kDirNorth;
   else if (dwKey & kKeyDown)
      dir = kDirSouth;
   else if (dwKey & kKeyLeft)
      dir = kDirWest;
   else if (dwKey & kKeyRight)
      dir = kDirEast;

   g_InputState.dwKeyPress = dwKey;
   if (dir != kDirUnknown || g_InputState.dir != kDirUnknown)
   {
      g_InputState.prevdir = g_InputState.dir;
      g_InputState.dir = dir;
   }
}

VOID
PAL_BridgeConfirm(
   INT fOn
)
{
   g_fConfirm = fOn ? 1 : 0;
}

VOID
PAL_BridgeEquipMenu(
   INT iItem,
   INT iSel
)
{
   g_nEquipItem = iItem;
   g_nEquipSel = iSel;
}

VOID
PAL_BridgeItemMenu(
   INT fOn
)
{
   g_fItemMenu = fOn ? 1 : 0;
}

VOID
PAL_BridgeWalk(
   VOID
)
/*++
  Purpose:

    Re-assert the walking DIRECTION right where PAL_UpdateParty() reads it.

    Direction only. This runs before PAL_StartFrame() checks for Space, so
    rewriting dwKeyPress here (it used to call _bridge_setkey) wiped a
    one-frame search before the engine ever looked at it: the press was
    silently lost, and longer presses only worked because they happened to
    still be the current action.

--*/
{
   DWORD dwKey = kKeyNone;
   int   dir = kDirUnknown;

   if (!g_fEnabled)
      return;
   g_nWorldFrames++;
   if (g_iAction < g_nActions)
      dwKey = g_rgActions[g_iAction].dwKey;

   if (dwKey & kKeyUp)
      dir = kDirNorth;
   else if (dwKey & kKeyDown)
      dir = kDirSouth;
   else if (dwKey & kKeyLeft)
      dir = kDirWest;
   else if (dwKey & kKeyRight)
      dir = kDirEast;

   if (dir != g_InputState.dir)
   {
      g_InputState.prevdir = g_InputState.dir;
      g_InputState.dir = dir;
   }
}

INT
PAL_BridgeTakeRecovery(
   VOID
)
/*++
  Purpose:
    Consume (and clear) the one-shot recovery-load flag. Called by
    synchronous "waiting for a key" loops so a {"load":N} sent while
    a plot script is blocked can actually take effect.

  Return value:
    1 if a recovery load is pending, 0 otherwise.
--*/
{
   // Consume ONE unit of the bounded budget, not the whole flag:
   // each PAL_DialogWaitForKey loop in the blocked script needs its
   // own break. Expiring after BRIDGE_RECOVERY_BUDGET keeps normal
   // dialogs untouched.
   if (g_nBridgeRecoveryBudget > 0)
   {
      g_nBridgeRecoveryBudget--;
      return 1;
   }
   return 0;
}

VOID
PAL_BridgePump(
   VOID
)
{
   // Every "waiting for the player" loop funnels through here, in BOTH
   // the agent run and a normal human game. Sample the engine's own state
   // for the human play log before the agent-only early return.
   PAL_HumanLogFrame();

   // LISTEN mode, no agent attached yet: the game runs standalone (GUI on,
   // human keyboard works). Poll for a new agent once per frame; a fresh
   // connection re-handshakes and takes over the CURRENT state, so an agent
   // can be attached at ANY point of the game, repeatedly.
   if (g_fListen && !g_fEnabled)
   {
      _bridge_try_accept();
      if (!g_fEnabled)
         return;
   }

   if (!g_fEnabled)
      return;

   if (g_nFrameMs > 0)
      SDL_Delay(g_nFrameMs);
   g_nFrame++;
   g_dwTicks += 16;      // one pump ~= one 60 Hz frame of game time

   if (g_iAction < g_nActions)
   {
      //
      // Serve the queued action: hold the key for the requested number of
      // input pumps, exactly like a player holding a key down.
      //
      BRIDGEACTION *p = &g_rgActions[g_iAction];
      _bridge_setkey(p->dwKey);
      g_nServed++;
      if (--p->nFrames <= 0)
         g_iAction++;
      return;
   }

   if (g_fQuitRequested)
   {
      // LISTEN mode: "quit" means the agent is detaching, NOT that the game
      // should shut down. Drop the connection, keep the world state, go back
      // to accept() so another agent can reattach.
      if (g_fListen)
      {
         g_fEnabled = 0;
         g_fQuitRequested = 0;
         PAL_CLOSE(g_sock);
         g_sock = PAL_INVALID_SOCK;
         g_nInLen = 0;
         g_nActions = 0;
         g_iAction = 0;
         _bridge_setkey(kKeyNone);
         _bridge_clock_domain_reset();
         fprintf(stderr, "BRIDGE agent quit (detached); "
                         "game continues, waiting for next agent\n");
         return;
      }
      _bridge_setkey(kKeyMenu);
      return;
   }

   //
   // Queue is dry: this is a decision point. Report the world and block
   // until the agent says what to do next.
   //
   {
      size_t n = _bridge_state(g_szOut, sizeof(g_szOut));
      if (n + 2 > sizeof(g_szOut))
         n = sizeof(g_szOut) - 2;
      g_szOut[n] = '\n';
      g_szOut[n + 1] = '\0';
   }

   if (!_bridge_send(g_szOut))
   {
      // agent gone: stop interfering and let the game run normally.
      // LISTEN mode: next pump returns to accept() polling, state kept.
      g_fEnabled = 0;
      PAL_CLOSE(g_sock);
      g_sock = PAL_INVALID_SOCK;
      g_nInLen = 0;
      g_nActions = 0;
      g_iAction = 0;
      _bridge_setkey(kKeyNone);
      _bridge_clock_domain_reset();
      if (g_fListen)
      {
         fprintf(stderr, "BRIDGE agent detached (send failed); "
                         "waiting for next agent\n");
      }
      return;
   }

   char *pszLine = _bridge_recv_line();
   if (pszLine == NULL)
   {
      //
      // The agent is gone (EOF or a dead socket).
      // LISTEN mode: this is a normal DETACH. Keep the game state in memory
      // and go back to polling accept(), so the next agent can take over
      // from exactly this point. USER 10:11 "任意状态可接管".
      // Legacy connect mode: an engine that carried on here used to spin
      // uncontrolled forever, so keep the original exit(0).
      //
      g_fEnabled = 0;
      PAL_CLOSE(g_sock);
      g_sock = PAL_INVALID_SOCK;
      g_nInLen = 0;
      g_nActions = 0;
      g_iAction = 0;
      _bridge_setkey(kKeyNone);
      _bridge_clock_domain_reset();
      if (g_fListen)
      {
         fprintf(stderr, "BRIDGE agent detached; game continues, "
                         "state kept, waiting for next agent\n");
         return;
      }
      exit(0);
   }

   _bridge_parse(pszLine);

   if (g_nActions == 0)
   {
      // nothing to do: idle one pump so the engine keeps animating
      _bridge_setkey(kKeyNone);
      return;
   }

   _bridge_setkey(g_rgActions[0].dwKey);
   g_nServed++;
   if (--g_rgActions[0].nFrames <= 0)
      g_iAction = 1;
}
