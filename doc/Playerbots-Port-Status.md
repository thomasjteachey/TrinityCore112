# Playerbots Port Status (TrinityCore)

Last updated: 2026-04-01

## Goal
Bring autonomous offline playerbot battleground participation to this TrinityCore fork.

## Current state

- [x] BG queue deficit hook from queue update to script layer (`OnBattlegroundQueueNeedBots`).
- [x] WSG bridge script that can queue configured connected fillers.
- [x] Configured filler metadata loading from `characters` table (guid/race/team).
- [x] Pending deficit tracker with periodic retry loop for fillers that come online later.
- [x] `OfflineBotSessionManager` skeleton + candidate registry + request hook from WSG bridge.
- [x] `OfflineBotSessionManager` pending request queue + update loop placeholder.
- [x] `OfflineBotSessionManager` config + attempt/success telemetry counters.
- [x] Offline precheck pass (DB online-state + retry cooldown) before headless start placeholder.
- [x] Headless-start callback interface wired (`RegisterHeadlessStartCallback`).
- [ ] Offline headless bot session creation (core runtime).
- [ ] Autonomous AI update loop for offline sessions.
- [ ] Offline invite acceptance / battleground enter flow.
- [ ] BG objective/combat behaviors for autonomous bots.

## What was done in this step

1. Added pending-per-bracket deficit tracking for alliance/horde in the WSG bridge.
2. Added periodic retry pass (`OnUpdate`) every 5s to auto-queue newly connected eligible fillers.
3. Kept queue scheduling updates wired after successful retry queue insertions.

## Next implementation step

Implement phase-2 internals for `OfflineBotSessionManager`: replace `TryStartHeadlessSession` placeholder with real headless session boot + lifecycle ownership + queue join.
