/* -*- mode: c; tab-width: 4; c-basic-offset: 4; c-file-style: "linux" -*- */
//
// bridge.h -- the agent bridge.
//
// The whole point of this file: let a program play PAL without looking at
// pixels. Everything the agent needs is already in the engine's globals, so
// the bridge exports them and lets the agent drive the *real* input path
// (only movement still goes through direction keys, exactly like a player).
//
// Design notes (see docs in the agent repo):
//   * OFF by default. The game behaves exactly as before unless PAL_AGENT=1.
//   * PAL_ProcessEvent() is the decision point: every place the engine waits
//     for the player goes through it, so pumping input from there covers
//     walking, dialogs, menus, shops and the battle UI in one hook.
//   * The agent is the scheduler: when the action queue runs dry the engine
//     sends a state snapshot and blocks for the next batch. That makes runs
//     deterministic and replayable from the input log.
//
#ifndef BRIDGE_H
#define BRIDGE_H

#include "common.h"

PAL_C_LINKAGE_BEGIN

//
// Called once at startup. Reads:
//   PAL_AGENT=1              enable the bridge (default: off)
//   PAL_AGENT_PORT=N         TCP port to connect to (default 4567)
//   PAL_AGENT_HOST=addr      host to connect to (default 127.0.0.1)
//   PAL_AGENT_SEED=N         RNG seed (default 1; 0 keeps the old behaviour)
//   PAL_AGENT_FAST=0         keep real-time delays (default: skip them)
//
VOID
PAL_BridgeInit(
   VOID
);

//
// Opt-in HUMAN PLAY RECORDER (USER 22:48: "你先加引擎日志 ... 我玩的
// 记录可以让你学习"). Independent of PAL_AGENT: set PAL_HUMAN_LOG=<path>
// and the NORMAL windowed game appends newline-delimited JSONL of every
// key press and every scene / position / facing / battle change. The
// agent can replay and learn from the real player's route without ever
// taking control of the keyboard.
//
VOID
PAL_HumanLogInit(
   VOID
);

VOID
PAL_HumanLogFrame(
   VOID
);

INT
PAL_BridgeEnabled(
   VOID
);

//
// TRUE when real-time delays must be skipped. This is what gets the engine
// from 60 fps to thousands of frames per second; it is also why every delay
// site in the engine has to ask this instead of sleeping blindly.
//
INT
PAL_BridgeFast(
   VOID
);

//
// TRUE when drawing can be skipped entirely (headless runs).
//
INT
PAL_BridgeNoRender(
   VOID
);
INT
PAL_BridgeAudio(
   VOID
);
INT
PAL_BridgeCinematics(
   VOID
);

INT
PAL_BridgeBootSlot(
   VOID
);

//
// Advance the virtual clock by `ms` of game time.
//
// Every "wait" in the engine -- UTIL_Delay, PAL_DelayUntil, the frame limiter
// -- has to mean the same thing in agent mode: move game time forward, and
// give the agent exactly one chance to answer. Patching them one at a time
// left them disagreeing (one loop spun without moving the clock, another
// stopped reading the socket altogether) and the engine and agent ended up
// waiting for each other.
//
void
PAL_BridgeAdvance(
   unsigned int ms
);

//
// Replacement for SDL_Delay(): advances the virtual clock instead of really
// sleeping.
//
// Waiting loops that only call SDL_Delay() and SDL_GetTicks() never reach
// PAL_ProcessEvent(), so with a virtual clock nothing advances them and the
// engine hangs -- which is exactly what happened during the opening cutscene.
// Making SDL_Delay move the clock forward keeps those loops finite without
// adding round trips to the agent.
//
void
PAL_BridgeDelay(
   unsigned int ms
);

//
// Replacement for SDL_GetTicks(): a virtual clock in agent mode, the real
// one otherwise. Wired up with a macro in common.h so every call site in
// the engine picks it up.
//
unsigned int
PAL_BridgeTicks(
   void
);

//
// Called from PAL_ProcessEvent(): apply the next queued key, or, when the
// queue is empty, report the state and wait for the agent's next batch.
//
VOID
PAL_BridgePump(
   VOID
);

//
// Called when the engine has consumed the key the bridge was holding, so the
// current agent action can be retired instead of being overwritten.
//
//
// Dialog state. The engine knows exactly when a dialog is on screen -- the
// agent needs that because pressing a direction key while a line is showing
// does nothing at all, which reads as "stuck" if you cannot tell.
//
//
// The message number of the line currently being shown, recorded where the
// script's 0xFFFF instruction picks the text (script.c, "Print dialog text":
// PAL_GetMsg(rgwOperand[0])). NOT PAL_StartDialog()'s byte argument -- that
// one really is the font colour. Knowing the number turns "something was
// said" into what was said.
//
VOID
PAL_BridgeDialogIndex(
   INT iIndex
);

//
// Bracket PAL_DialogWaitForKeyWithMaximumSeconds(): the engine is blocked
// until a key is pressed. This, not "a dialog was started", is what the
// agent must know -- a script can print line after line with bare 0xFFFF
// instructions after a single StartDialog, and each of them stops for a key.
//
VOID
PAL_BridgeWaitKey(
   INT fWaiting
);

VOID
PAL_BridgeDialogBegin(
   VOID
);

VOID
PAL_BridgeDialogEnd(
   VOID
);

INT
PAL_BridgeDialog(
   VOID
);

//
// Re-assert the held key right before the party is moved.
//
// PAL_UpdateKeyboardState() runs inside PAL_ProcessEvent() and clears
// g_InputState.dir whenever no *physical* key is down -- which is always, in
// agent mode. So setting the direction in PAL_ProcessEvent() is undone before
// PAL_UpdateParty() reads it. Calling this at the top of PAL_UpdateParty()
// puts the write where it is actually consumed.
//
VOID
PAL_BridgeWalk(
   VOID
);

VOID
PAL_BridgeMark(
   INT iUI
);

//
// Bracket PAL_ConfirmMenu(): a yes/no box is up, and it is a question for the
// agent -- cursor starts on No, Right moves to Yes, Search confirms.
//
VOID
PAL_BridgeConfirm(
   INT fOn
);

VOID
PAL_BridgeEquipMenu(
   INT iItem,
   INT iSel
);

VOID
PAL_BridgeItemMenu(
   INT fOn
);

VOID
PAL_BridgeConsumed(
   VOID
);

//
// Force the LCG in util.c to a known seed. Returns the seed actually used.
//
INT
PAL_BridgeSetSeed(
   INT iSeed
);

PAL_C_LINKAGE_END

#endif
