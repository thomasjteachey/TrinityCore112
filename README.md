# Centurion

Centurion is a World of Warcraft PvP server for the 3.3.5a client, capped at level 60
with Outland and Northrend closed. This repository holds everything needed to run your
own copy: the server source, its databases, the client patches and DLL, and the
launcher players start the game with.

- **Two kinds of character.** World characters level up under hardcore rules: full-loot
  death chests, field kits and War Mode. Tournament characters start at 60 and play only
  battlegrounds and arenas.
- **Custom battlegrounds**, among them the Obsidian Colosseum, Tanaris, Violet Hold,
  Scarlet Chapel and Blackrock Throne.
- **A world full of bots**: zone guardians, drifters and bounty hunters out in the world,
  and battleground fill bots in easy, medium and hard tiers.
- Bounties and notoriety, challenge modes, arena replays, and dungeons that scale to the
  size of your group.

It is built on [TrinityCore](https://github.com/TrinityCore/TrinityCore) 3.3.5.

## What is in this repository

The Centurion code is on the **`CENTURION`** branch. `master` is an old snapshot. When
you fork on GitHub, untick **Copy the `master` branch only**.

| folder | what |
|---|---|
| `src/` | the server: TrinityCore plus Centurion's changes and the playerbot module |
| `centurion/` | what the server runs on: databases, DBCs, client patches, client DLL, launcher source, and the live bot and dungeon-scaling configs |
| `sql/` | TrinityCore's SQL tree, plus some of Centurion's own changes in `sql/custom`; the databases in `centurion/sql/` already contain them |
| `clientedits/` | source for Centurion's addons, including the GM-only ones, and interface files |
| `tools/` | maintenance scripts: map data, DBC editing, battleground builders |
| `doc/`, `docs/` | design notes |
| `jenkins/` | the build pipeline the live realm uses |
| `playerbot reference/` | a reference copy of another playerbot project; not compiled |

## Setting up your own Centurion

You need:

- a machine for the servers: Ubuntu 22.04 (what Centurion runs on) or Windows 10/11
- MySQL 8.0; MariaDB will not load these tables
- a web server that serves static files over HTTPS, for the client patches
- a clean enUS World of Warcraft 3.3.5a (build 12340) client, for extracting map data
  and for playing

### 1. Install the build tools

**Ubuntu 22.04**

```bash
sudo apt update
sudo apt install git clang cmake make gcc g++ libmysqlclient-dev libssl-dev libbz2-dev libreadline-dev libncurses-dev libboost-all-dev zlib1g-dev mysql-server
```

The build checks for at least CMake 3.11, GCC 10 or Clang 11, and Boost 1.71. Ubuntu
22.04 ships newer versions of all of them.

**Windows**

- Visual Studio 2022 with the **Desktop development with C++** workload.
- CMake 3.11 or newer. Visual Studio includes one.
- Git for Windows. It also gives you Git Bash, which runs the `.sh` scripts here.
- Boost 1.73 or newer, prebuilt for MSVC 14.3 64-bit. Centurion builds with 1.78,
  installed to `C:\local\boost_1_78_0`.
- OpenSSL 3, from the full **Win64 OpenSSL** installer (not "Light").
- MySQL Server 8.0. The build links against the `include` and `lib` folders in its
  install directory.

### 2. Build the server

```bash
git clone -b CENTURION https://github.com/<you>/TrinityCore112.git
cd TrinityCore112
```

**Linux:**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=$HOME/centurion-server -DPLAYERBOT=ON -DTOOLS=ON
cmake --build build -j"$(nproc)"
cmake --install build
```

This installs the programs into `~/centurion-server/bin` and the `.conf.dist` files into
`~/centurion-server/etc`.

**Windows:**

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DPLAYERBOT=ON -DTOOLS=ON -DBOOST_ROOT=C:/local/boost_1_78_0
cmake --build build --config RelWithDebInfo
```

The programs and `.conf.dist` files land in `build\bin\RelWithDebInfo`. Copy these DLLs
next to `worldserver.exe`:

- `libmysql.dll` from `C:\Program Files\MySQL\MySQL Server 8.0\lib`
- `libcrypto-3-x64.dll` and `libssl-3-x64.dll` from `C:\Program Files\OpenSSL-Win64\bin`

`-DPLAYERBOT=ON` is required. It defaults to off, and without it the bots are not
compiled in at all. The build takes a while; the bot module alone is large.

### 3. Load the databases

Create the account the servers log in with. This matches the `trinity` user in the
`.conf.dist` files; pick your own password:

```sql
CREATE USER 'trinity'@'localhost' IDENTIFIED BY 'choose-a-password';
```

Load the snapshot as a user that can create databases (root):

```bash
cd centurion/sql
MYSQL="mysql -u root -p" ./import.sh
```

Then let `trinity` use them:

```sql
GRANT ALL PRIVILEGES ON auth.* TO 'trinity'@'localhost';
GRANT ALL PRIVILEGES ON world.* TO 'trinity'@'localhost';
GRANT ALL PRIVILEGES ON characters.* TO 'trinity'@'localhost';
```

`import.sh` creates `auth`, `world` and `characters`, the names the `.conf.dist` files
already use. To pick other names, set `AUTH_DB`, `WORLD_DB` or `CHAR_DB`. The script
renames the database references inside triggers, views and procedures to match. It
loads Centurion's 259 bots too; set `BOTS=0` to start without them. It refuses to
overwrite a database that already has tables unless you set `FORCE=1`. On Windows, run
it from Git Bash.

### 4. Set up the client patches

Rebuild the three zips that are stored in pieces, and check all of them:

```bash
cd centurion/patches
./join.sh
```

On Windows without Git Bash, run `join.ps1` in PowerShell instead.

**Host them.** Copy every `*.zip` and `*.version` from `centurion/patches/` to
`<web root>/downloads/patches/` on your web server. You don't need the `.partNN` files.
The launcher downloads `<update URL>/patches/<name>.zip`, and downloads it again only when
`<name>.version` changes. To ship a changed patch, replace the zip first, then raise the
number in its `.version` file. Keep the format `1.00042`, with no newline at the end.
The optional HD packs come from Centurion's server instead; see
[The HD packs](#the-hd-packs).

**Install them into a client.** Step 5 needs a client that has the patches. Either run
your launcher once (step 8), or unpack them by hand:

| zip | contains | unpack into |
|---|---|---|
| `patch-Y.zip` | the art base, `patch-Y.MPQ` | `Data\`, then rename it to `patch-X.MPQ` |
| `patch-Z.zip` | `patch-Z.MPQ` | `Data\` |
| `patch-dungeon-maps.zip` | `patch-M.MPQ`, dungeon floor maps | `Data\` |
| `patch-enUS-6.zip` | login screen, character select and create | `Data\enUS\` |
| `patch-enUS-7.zip` | interface | `Data\enUS\` |
| `patch-enUS-A.zip` | Centurion's client DBCs and interface | `Data\enUS\` |
| `addons.zip` | Centurion addons | `Interface\AddOns\` |
| `client-tweaks.zip` | `dinput8.dll` | the client folder, next to `Wow.exe` |

The extractors in step 5 read the archives directly, so this is enough for them. Playing
also needs a patched `Wow.exe`: the patches replace interface files that a stock
`Wow.exe` checks against Blizzard's signatures. The launcher removes that check on every
start (step 8).

### 5. Set up the server data

```bash
mkdir -p ~/centurion-server/data/dbc
cp centurion/dbc/*.dbc ~/centurion-server/data/dbc/
```

Use these DBCs, not the ones the extractor writes. The client's copies in
`patch-enUS-A` differ from the server's in places.

For `maps`, `vmaps` and `mmaps`, copy the four tools from step 2 into a client folder that
has the patches from step 4, and run them there:

```bash
./mapextractor
./vmap4extractor
mkdir vmaps && ./vmap4assembler Buildings vmaps
mkdir mmaps && ./mmaps_generator
```

The extractors read the highest-priority copy of every file in the client. On a client
without the Centurion patches, the custom maps come out stock. `mmaps_generator` takes
hours. When it finishes, move `maps`, `vmaps`, `mmaps` and `Cameras` into
`~/centurion-server/data/`.

### 6. Configure

In the folder with the `.conf.dist` files:

- Copy `worldserver.conf.dist` to `worldserver.conf` and `authserver.conf.dist` to
  `authserver.conf`.
- Copy `centurion/conf/playerbots.conf` and `centurion/conf/AutoBalance.conf` next to
  `worldserver.conf`. They are the live realm's. `playerbots.conf` names the bot accounts
  and battleground tiers from step 3 and keeps 150 bots online. Use `playerbots.conf.dist`
  instead if you loaded with `BOTS=0`.

In `worldserver.conf`:

- `DataDir`: the folder from step 5, for example `"/home/you/centurion-server/data"`.
- `LoginDatabaseInfo`, `WorldDatabaseInfo` and `CharacterDatabaseInfo`: replace the
  second `trinity` (the password) with yours.
- `Updates.EnableDatabases = 0`. These databases carry Centurion's own changes, so
  TrinityCore's auto-updater must not replay stock updates over them.
- To play like the live realm: `MaxPlayerLevel = 60`, `GameType = 1` (PvP),
  `Centurion.Hardcore.Enable = 1` and `Centurion.Tournament.Enable = 1`. The `.dist`
  file has 80, 0, 0 and 0.

In `authserver.conf`, set the password in `LoginDatabaseInfo` the same way.

Every Centurion key is documented in the `.dist` files, so read through them and turn on
what you want.

### 7. Start the servers

Start `authserver`, then `worldserver`, each from the folder it is in. On Linux, run them
under `screen`, `tmux` or systemd so they keep running after you log out. When the
worldserver console is ready, create your account and make it a GM:

```text
account create <name> <password>
account set gmlevel <name> 3 -1
```

The bot accounts use ids 76 to 79, so the first account you create gets id 80.

The auth database has one realm, id 1 "Centurion" at `127.0.0.1:8085`. That only works
from the same machine. For other players, give it your public address, and your LAN
address for players on your own network:

```sql
UPDATE auth.realmlist SET address = 'play.example.com', localAddress = '192.168.1.10' WHERE id = 1;
```

Open or forward TCP 3724 (authserver) and 8085 (worldserver).

### 8. Set up the launcher

Players start the game through the launcher. It downloads the patches and
`dinput8.dll`, patches `Wow.exe` (build number 12342 and the signature check from step
4), writes the realmlist into `Config.wtf`, and starts the game. The realm row says
build 12342, and authserver shows a realm built for another client build as offline, so
a stock client cannot get in.

**You don't have to build it.** Centurion's own launcher,
`https://centurionpvp.com/downloads/centurionlauncher.zip`, works with any server. Players
put `CenturionLauncher.exe` next to `Wow.exe`, open **Settings** (top bar) and change two
fields:

- **Trinitycore realmlist server**: your authserver's address.
- **Game update server**: where you host the patches, for example
  `https://play.example.com/downloads/`. Use a hostname with a valid certificate. An
  `https://` address with a bare IP fails, because the launcher then checks the
  certificate against `centurionpvp.com`. Plain `http://` works for patches, but the
  launcher only updates itself over HTTPS.

The launcher picks the realm named `Centurion`, which is what the realmlist row from
step 7 is called, so keep that name. The settings are saved in `.launcher\settings.json`
next to the launcher, so you can also hand players a ready-made one. The launcher still lists the HD packs, so host them
too (see [The HD packs](#the-hd-packs)); otherwise a player who switches one on gets a
download error. If you don't host `centurionlauncher.zip` and `centurionlauncher.version`,
the launcher says its auto-update is unavailable and keeps working.

A player whose client already came from Centurion keeps its patches. Changing the update
server only makes the launcher compare versions, and it downloads a patch again only
when your `.version` file differs from the one it installed. The zips and `.version`
files in this repository match what Centurion served on 2026-09-26.

**To build your own**, for your own name, defaults or patch list, point it at your
servers in `centurion/launcher/src/common/constants.ts`:

- `DEFAULT_LAUNCHER_UPDATE_URL`: your `https://<host>/downloads/`. The launcher checks
  the TLS certificate against this hostname.
- `DEFAULT_REALMLIST`: your authserver's address.
- In `FileMap`, delete the HD entries (`hd-creatures`, `hd-textures`, `hd-spells`,
  `hd-bgs`, `hd-misc`, `world-terrain`) unless you host those packs. The `centurion`
  realm's `realmName` must match `realmlist.name` (`Centurion`) exactly.

Then build it on Windows. Centurion's is built with Node.js 24:

```powershell
cd centurion/launcher
npm install
npm run dist
```

This produces `dist\CenturionLauncher.exe` and `dist\centurionlauncher.zip`. Players put
the exe in their client folder, next to `Wow.exe`, and run it. For launcher self-updates,
upload the zip to `<web root>/downloads/` beside a `centurionlauncher.version` file; see
`centurion/launcher/docs/self-update.md`.

The client DLL (`centurion/client/dinput8.dll`, also inside `client-tweaks.zip`) is
prebuilt, so there is nothing to compile for it. Its source is in
[thomasjteachey/clientedits](https://github.com/thomasjteachey/clientedits).

## What the databases hold

Snapshot taken 2026-09-26 from the live realm.

**World: all the game data.** One file per table under `centurion/sql/world/`.
`broadcast_text_locale` is split into `.1.sql` and `.2.sql` so every file stays under
GitHub's 50 MB warning. About 216 backup and scratch tables from the live database are
left out (`zz_*`, `*_bak_*`, `tmp_*`, `*_backup`, `*_copy` and similar). The two `zz_`
tables the server still reads, `zz_fieldkit_map` and `zz_tmode_item_map`, are included.
The nine procedures in `_routines.sql` are maintenance tools, and several of them expect
a `classicmangos` database you will not have. The server never calls them.

**Characters: empty tables, plus two seeds.**

- `characters_seed.sql`: the talent builds bots use (`playerbot_talent_recipe`), and the
  nine tournament template characters (`Startwarrior`, `Startmage` and so on) with their
  spells, talents, action bars and hunter pets. The `createTournamentKit` procedure
  copies from them when a tournament character is created. They belong to account 0, so
  nobody can log them in.
- `characters_bots.sql`: all 259 bots with their gear, spells, talents, glyphs, quests,
  reputation, skills, action bars, hunter pets and homebinds. It also has the two bot
  guilds (AI Uprising and The Robot Masters) and the zone-guardian posts. The 27 named
  PvP bots on account 79 fill battlegrounds. Their guids are the same as on the live
  realm, because the easy, medium and hard tiers in `playerbots.conf` pick bots by guid.

**Auth: empty tables, plus the reference data** that authserver and worldserver need to
work: RBAC permissions, client build info and hashes, and the `updates` history. There
is also the one `realmlist` row from step 7. `auth_bots.sql` adds the four bot accounts.
Bots log in inside the worldserver, never through authserver, so their passwords are
random bytes nobody knows.

No player accounts, player characters, mail, auction data or logs are included.

## About the patches and addons

The zips in `centurion/patches/` are byte-identical to what the live download server
served on 2026-09-26, and `patches.md5` has their checksums. `patch-Y.zip`,
`patch-enUS-6.zip` and `patch-enUS-A.zip` are stored as 49 MB pieces, because GitHub
refuses files over 100 MB. `join.sh` or `join.ps1` puts them back together. The joined
zips are git-ignored so they cannot be committed by accident.

`patch-Y.zip` keeps its old name because the launcher and the Centurion download server
still use it. The launcher writes it to disk as `patch-X.MPQ`, which frees the `Y` slot
for the optional World Terrain pack.

`addons.zip` is the addon pack every player gets, because the launcher treats it as
required. It holds the 17 `CENTURION_*` addons (character pane, class boot, diminishing
returns, energy ticker, Gurubashi timer, heirloom colours, key binding profiles,
Mok'gora, replays, scoreboard, tooltips, WSG helper, zone bands, bot map, auto-auction,
temporal tint, default options), TrinketMenu, Cooldowns, and Centurion's versions of
`Blizzard_TalentUI` and `Blizzard_BattlefieldMinimap`. Two GM-only addons are not in it,
because players never get them: `CENTURION_GMOnline` (the GM Panel, whose Server page
reads `.tick addon`) and `CENTURION_BotStats`. They are in `clientedits/addons/`. Copy
them into a GM's `Interface\AddOns\` by hand.

## The HD packs

Players can switch on six optional graphics packs in the launcher. Together they are
2.5 GB, so they are not in this repository. Download them from Centurion's server: for
each pack, `https://centurionpvp.com/downloads/patches/<zip>` and the `.version` file
with the same name. Put both next to your other patches in
`<web root>/downloads/patches/`, and keep the `.version` files unchanged so players who
already have a pack don't download it again.

| launcher toggle | zip | size | installs as | what it changes |
|---|---|---|---|---|
| HD Creatures | `hd-creatures.zip` | 1.45 GB | `Data\patch-F.MPQ` | higher-resolution retail creature models |
| HD Textures | `hd-textures.zip` | 418 MB | `Data\patch-T.MPQ` | higher-resolution retail textures |
| HD Spells | `hd-spells.zip` | 83 MB | `Data\patch-G.MPQ`, `Data\patch-H.MPQ` | higher-resolution retail spell visuals |
| HD Battlegrounds | `hd-bgs.zip` | 233 MB | `Data\patch-U.MPQ` | more detailed retail battleground maps |
| HD Interface | `hd-misc.zip` | 122 MB | `Data\patch-L.MPQ` | a Shadowlands-style interface |
| Alt World | `world-terrain.zip` | 147 MB | `Data\patch-Y.MPQ` | Reznik's world textures, models, skyboxes and lighting |

The client ranks archives by the last character of their name, and letters outrank
Centurion's `patch-enUS-A`. So when a pack carries its own copy of a DBC, a player with
that pack on gets the pack's copy. If you change one of these DBCs, change it inside the
pack too, or players with the pack on won't see the change:

| pack | DBCs it carries |
|---|---|
| HD Creatures | CreatureDisplayInfo, CreatureDisplayInfoExtra, CreatureModelData, CreatureSoundData, ParticleColor, SoundEntries, SpellVisualEffectName |
| HD Spells | SpellVisual, SpellVisualKit, SpellVisualKitModelAttach, SpellVisualEffectName, SpellChainEffects, SpellMissile, SpellMissileMotion, SoundEntries |
| HD Interface | LoadingScreens |
| Alt World | Light |

HD Textures and HD Battlegrounds carry no DBCs.

## Not included

- **The optional HD packs.** See [The HD packs](#the-hd-packs) for where to get them.
- **`maps`, `vmaps`, `mmaps`.** Generate them in step 5.
- **The live `worldserver.conf`.** It holds passwords. Step 6 lists the settings that
  make a realm play like Centurion; everything else is documented in the `.dist` file.

The DLL can attest to the server (`ClientTweaks.Attest.*`, off by default). Its shared
secret is compiled into the DLL and is public in the clientedits source. If you turn
attestation on, rebuild the DLL with your own secret and set the same value in
`worldserver.conf`.

## License

Centurion is a modified TrinityCore, and like TrinityCore it is licensed under the GNU
General Public License v2; see [COPYING](COPYING). TrinityCore's contributors are listed
in [AUTHORS](AUTHORS).
