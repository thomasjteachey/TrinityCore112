# AutoBalance Module (TrinityCore 3.3.5a)

The AutoBalance module scales non-player creatures based on the active party size. TrinityCore integrates the module directly into the core build system and exposes configuration and runtime controls that differ from the upstream project.

## Configuration template

* The template configuration file is installed as `conf/AutoBalance.conf.dist` alongside `worldserver.conf.dist`.  Set `AutoBalance.Conf` in `worldserver.conf` if you want to relocate the runtime configuration file or disable loading entirely. 【F:cmake/InstallConfig.cmake†L1-L8】【F:src/server/worldserver/worldserver.conf.dist†L3109-L3114】

## Reloading configuration at runtime

* After updating `AutoBalance.conf`, run `.reload autobalance` from the console or an authorized GM character.  This triggers an in-game reload without requiring a worldserver restart. 【F:src/server/scripts/Commands/cs_reload.cpp†L24-L39】【F:src/server/game/AutoBalance/AutoBalanceConfig.cpp†L178-L204】

## Logging

* AutoBalance messages use the `module.AutoBalance` log filter. Enable it in `AutoBalance.conf` or `worldserver.conf` if you need verbose diagnostics. 【F:src/server/game/AutoBalance/AutoBalanceConfig.h†L11-L49】

## Source layout

* Core logic lives under `src/server/game/AutoBalance/` and custom script bindings under `src/server/scripts/Custom/AutoBalance/`.  The default script module registers automatically with the TrinityCore script loader. 【F:src/server/game/CMakeLists.txt†L50-L52】【F:src/server/scripts/Custom/custom_script_loader.cpp†L19-L27】
