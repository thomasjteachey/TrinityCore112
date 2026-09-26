# Centurion bundle

Everything outside the C++ source that you need to run your own Centurion realm from
this repository. The server code is this branch (`CENTURION`). This folder holds the
data it runs on, the client DLL and the launcher.

Snapshot taken 2026-09-26 from the live Centurion realm.

| folder | what | source |
|---|---|---|
| `sql/` | the three databases, plus `import.sh` | live `centurionworld`, `centurioncharacters`, `legionnaireauth` |
| `dbc/` | 246 server DBC files (97 MB) | the live worldserver's `data/dbc` |
| `client/dinput8.dll` | client-tweaks 1.00017 (md5 `6d8d54cf174fee4a0eb06f1ae5286dc1`) | the build players download; source is [thomasjteachey/clientedits](https://github.com/thomasjteachey/clientedits) at `908875a` |
| `launcher/` | CenturionLauncher 1.1.23 source (Electron) | [thomasjteachey/centurionlauncher](https://github.com/thomasjteachey/centurionlauncher) at `1a92391` |

> On GitHub, the fork dialog copies only the default branch unless you untick
> **Copy the `master` branch only**. This bundle and the Centurion code are on `CENTURION`.

## What is in the databases

**World: all the game data.** One file per table under `sql/world/`. `broadcast_text_locale`
is split into `.1.sql` and `.2.sql` so every file stays under GitHub's 50 MB warning.
About 216 backup and scratch tables from the live database are left out (`zz_*`,
`*_bak_*`, `tmp_*`, `*_backup`, `*_copy` and similar). The two `zz_` tables the server still
reads, `zz_fieldkit_map` and `zz_tmode_item_map`, are included. The nine procedures in
`_routines.sql` are maintenance tools, and several of them expect a `classicmangos`
database you will not have. The server never calls them.

**Characters: empty tables, plus a small seed** (`characters_seed.sql`):

- `playerbot_talent_recipe` holds the talent builds bots use.
- The nine tournament template characters (`Startwarrior`, `Startmage` and so on) and
  their spells, talents, action bars and hunter pets. The `createTournamentKit` procedure
  copies from them when a tournament character is created. They belong to account 0, so
  nobody can log them in.

**Auth: empty tables, plus the reference data** that authserver and worldserver need to
work: RBAC permissions, client build info and hashes, and the `updates` history. There
is also one `realmlist` row: realm 1 "Centurion" on `127.0.0.1:8085`, client build 12342.

No accounts, characters, mail, guilds, auction data or logs are included.

## Setting up a realm

1. **Build the server** from this branch like any TrinityCore 3.3.5 build (CMake, then
   build `worldserver`, `authserver` and the tools in `src/tools`).

2. **Load the databases.** You need MySQL 8.0; MariaDB lacks the `utf8mb4_0900_ai_ci`
   collation these tables use.

   ```bash
   cd centurion/sql
   MYSQL="mysql -h 127.0.0.1 -u root -p" ./import.sh
   ```

   This creates `auth`, `world` and `characters`, the names the `.conf.dist` files already
   use. To pick other names, set `AUTH_DB`, `WORLD_DB` or `CHAR_DB`. The script renames the
   database references inside triggers, views and procedures to match. It refuses to
   overwrite a database that already has tables unless you set `FORCE=1`. On Windows,
   run it from Git Bash.

3. **Configure.** Copy `worldserver.conf.dist`, `authserver.conf.dist` and
   `playerbots.conf.dist` to `.conf` and edit them. Two settings must change:
   - `Updates.EnableDatabases = 0` in `worldserver.conf`. These databases carry
     Centurion's own changes, so TrinityCore's auto-updater must not replay stock updates
     over them. The live realm runs with 0.
   - `DataDir` must point at a folder containing `dbc/`, `maps/`, `vmaps/` and `mmaps/`.

   The dist files leave most Centurion features off, for example
   `Centurion.Tournament.Enable = 0` and `Playerbot.Enable = 0`. Each key is documented in
   place.

4. **Data files.** Copy `centurion/dbc/*.dbc` into `<DataDir>/dbc/`. Generate `maps`,
   `vmaps` and `mmaps` from a 3.3.5a client with the extractors built in step 1
   (`mapextractor`, `vmap4extractor`, `vmap4assembler`, `mmaps_generator`). The
   extractors read the highest-priority copy of every file in the client, so run them on
   a client with the Centurion patches installed. On a stock client, the custom maps
   come out stock.

5. **Accounts.** From the worldserver console run `account create <name> <password>`,
   then `account set gmlevel <name> 3 -1` for yourself.

6. **Client.** Put `client/dinput8.dll` next to `Wow.exe`, and point `realmlist.wtf` at
   your server, or use the launcher.

## The launcher

```bash
cd centurion/launcher
npm install
npm run dist
```

It points at the Centurion servers by default. For your own realm, edit
`src/common/constants.ts`:

- `DEFAULT_LAUNCHER_UPDATE_URL`: where it downloads self-updates and client patches from.
- `DEFAULT_REALMLIST`: your auth server's address.
- The realm list, and `FileMap`, which says which patch archives each realm installs.
  `realmName` must match `realmlist.name` in your auth database exactly.

It downloads patches as `<url>/patches/<name>.zip` plus a `<name>.version` file, and only
re-downloads when the version file changes.

## Not included

- **Client patches (the MPQs).** The server's DBCs contain custom spells, items and
  maps, and players need the matching client patches to see them: every `FileMap` entry
  in the launcher that lists `centurion` or lists no realms at all. That includes
  `patch-enUS-6`, `-7` and `-A`, the art base and the dungeon maps. They are several
  gigabytes, too big for git, and the launcher fetches them from the Centurion download
  server. To run your own realm, host copies yourself and point the launcher at them.
- **`maps`, `vmaps`, `mmaps`.** Generate them from the client, see step 4.
- **Bot characters.** World bots log in as existing characters on accounts you list in
  `playerbots.conf` (`Playerbot.RandomPopulation.BotAccountIds` and related keys). Create
  those accounts and characters yourself.
- **The live `.conf` files.** Use the `.dist` files; they document every Centurion key.

The DLL can attest to the server (`ClientTweaks.Attest.*`, off by default). Its shared
secret is compiled into the DLL and is public in the clientedits source. If you turn
attestation on, rebuild the DLL with your own secret and set the same value in
`worldserver.conf`.
