# AzerothCore playerbots reference snapshot

This folder contains **copied** reference files from the local `azerothcore-wotlk-Playerbot` tree.

Purpose:
- keep a direct in-repo copy of playerbots-related touchpoints while porting to TrinityCore,
- avoid moving/removing the original `azerothcore-wotlk-Playerbot` folder,
- make side-by-side diffing easier during BG-focused integration work.

Selection rule used for this snapshot:
- files under `azerothcore-wotlk-Playerbot/src/server/{game,database,apps}` containing one of:
  - `playerbot`
  - `playerbots`
  - `mod-playerbots`
  - `mod_playerbots`
  - `ai_playerbot`

The copied files are references only and are **not** compiled by TrinityCore.
