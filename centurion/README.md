# Centurion bundle

Everything outside the C++ source that you need to run your own Centurion realm. The
server code is this branch (`CENTURION`). This folder holds the data it runs on, the
client patches, the client DLL and the launcher. Nothing in it needs building except the
server and, if you want your own, the launcher.

Snapshot taken 2026-09-26 from the live Centurion realm.

| folder | what | source |
|---|---|---|
| `sql/` | the three databases, plus `import.sh` | live `centurionworld`, `centurioncharacters`, `legionnaireauth` |
| `dbc/` | 246 server DBC files (97 MB) | the live worldserver's `data/dbc` |
| `patches/` | every client patch Centurion players download (1.4 GB), ready to host | the live download server |
| `conf/AutoBalance.conf` | dungeon scaling settings; the repo has no `.dist` for it | the live realm |
| `client/dinput8.dll` | client-tweaks 1.00017, prebuilt (md5 `6d8d54cf174fee4a0eb06f1ae5286dc1`) | the build players download; source is [thomasjteachey/clientedits](https://github.com/thomasjteachey/clientedits) at `908875a` |
| `launcher/` | CenturionLauncher 1.1.23 source (Electron) | [thomasjteachey/centurionlauncher](https://github.com/thomasjteachey/centurionlauncher) at `1a92391` |

> On GitHub, the fork dialog copies only the default branch unless you untick
> **Copy the `master` branch only**. This bundle and the Centurion code are on `CENTURION`.

## Installing a Centurion server from scratch

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
refuses to overwrite a database that already has tables unless you set `FORCE=1`. On
Windows, run it from Git Bash.

### 4. Set up the client patches

Rebuild the three zips that are stored in pieces, and check all of them:

```bash
cd centurion/patches
./join.sh
```

On Windows without Git Bash, run `join.ps1` in PowerShell instead.

**Host them.** Copy every `*.zip` and `*.version` from `patches/` to
`<web root>/downloads/patches/` on your web server. You don't need the `.partNN` files.
The launcher downloads `<update URL>/patches/<name>.zip`, and downloads it again only when
`<name>.version` changes. To ship a changed patch, replace the zip first, then raise the
number in its `.version` file. Keep the format `1.00042`, with no newline at the end.

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

In the folder with the `.conf.dist` files, copy each one to a `.conf` without the `.dist`:
`worldserver.conf`, `authserver.conf` and `playerbots.conf`. `playerbots.conf` must sit
next to `worldserver.conf`, which is where the server looks for it. Also copy
`centurion/conf/AutoBalance.conf` next to `worldserver.conf`.

In `worldserver.conf`:

- `DataDir`: the folder from step 5, for example `"/home/you/centurion-server/data"`.
- `LoginDatabaseInfo`, `WorldDatabaseInfo` and `CharacterDatabaseInfo`: replace the
  second `trinity` (the password) with yours.
- `Updates.EnableDatabases = 0`. These databases carry Centurion's own changes, so
  TrinityCore's auto-updater must not replay stock updates over them. The live realm
  runs with 0.

In `authserver.conf`, set the password in `LoginDatabaseInfo` the same way.

The `.dist` files leave most Centurion features off, for example
`Centurion.Tournament.Enable = 0` and `Playerbot.Enable = 0`. Every key is documented in
place, so read through them and turn on what you want.

### 7. Start the servers

Start `authserver`, then `worldserver`, each from the folder it is in. On Linux, run them
under `screen`, `tmux` or systemd so they keep running after you log out. When the
worldserver console is ready, create your account and make it a GM:

```text
account create <name> <password>
account set gmlevel <name> 3 -1
```

The auth database has one realm, id 1 "Centurion" at `127.0.0.1:8085`. That only works
from the same machine. For other players, give it your public address, and your LAN
address for players on your own network:

```sql
UPDATE auth.realmlist SET address = 'play.example.com', localAddress = '192.168.1.10' WHERE id = 1;
```

Open or forward TCP 3724 (authserver) and 8085 (worldserver).

### 8. Build the launcher

Players start the game through the launcher. It downloads the patches and
`dinput8.dll`, patches `Wow.exe` (build number 12342 and the signature check from step
4), writes the realmlist into `Config.wtf`, and starts the game. The realm row says
build 12342, and authserver shows a realm built for another client build as offline, so
a stock client cannot get in.

Point it at your servers in `launcher/src/common/constants.ts`:

- `DEFAULT_LAUNCHER_UPDATE_URL`: your `https://<host>/downloads/`. The launcher checks
  the TLS certificate against this hostname.
- `DEFAULT_REALMLIST`: your authserver's address.
- In `FileMap`, delete the optional entries (`hd-creatures`, `hd-textures`, `hd-spells`,
  `hd-bgs`, `hd-misc`, `world-terrain`) unless you host those packs yourself; they are not
  in this bundle. The `centurion` realm's `realmName` must match `realmlist.name`
  (`Centurion`) exactly.

Then build it on Windows. Centurion's is built with Node.js 24:

```powershell
cd centurion/launcher
npm install
npm run dist
```

This produces `dist\CenturionLauncher.exe` and `dist\centurionlauncher.zip`. Players put
the exe in their client folder, next to `Wow.exe`, and run it. For launcher self-updates,
upload the zip to `<web root>/downloads/` beside a `centurionlauncher.version` file; see
`launcher/docs/self-update.md`.

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
is also the one `realmlist` row from step 7.

No accounts, characters, mail, guilds, auction data or logs are included.

## About the patches

The zips are byte-identical to what the live download server served on 2026-09-26, and
`patches.md5` has their checksums. `patch-Y.zip`, `patch-enUS-6.zip` and
`patch-enUS-A.zip` are stored as 49 MB pieces, because GitHub refuses files over 100 MB.
`join.sh` or `join.ps1` puts them back together. The joined zips are git-ignored so
they cannot be committed by accident.

**Addons.** `addons.zip` is the addon pack every player gets, because the launcher
treats it as required. It holds the 17 `CENTURION_*` addons (character pane, class boot,
diminishing returns, energy ticker, Gurubashi timer, heirloom colours, key binding
profiles, Mok'gora, replays, scoreboard, tooltips, WSG helper, zone bands, bot map,
auto-auction, temporal tint, default options), TrinketMenu, Cooldowns, and Centurion's
versions of `Blizzard_TalentUI` and `Blizzard_BattlefieldMinimap`. Two GM-only addons are
not in it, because players never get them: `CENTURION_GMOnline` (the GM Panel, whose
Server page reads `.tick addon`) and `CENTURION_BotStats`. They are in
`clientedits/addons/` at the root of this repository. Copy them into a GM's
`Interface\AddOns\` by hand.

`patch-Y.zip` keeps its old name because the launcher and the Centurion download server
still use it. The launcher writes it to disk as `patch-X.MPQ`, which frees the `Y` slot
for the optional World Terrain pack.

## Not included

- **The optional HD packs:** HD Creatures, Textures, Spells, Battlegrounds, Interface,
  and the Alt World pack. Together they are 2.5 GB. Remove them from the launcher as
  step 8 describes.
- **`maps`, `vmaps`, `mmaps`.** Generate them in step 5.
- **Bot characters.** World bots log in as existing characters on accounts you list in
  `playerbots.conf` (`Playerbot.RandomPopulation.BotAccountIds` and related keys). Create
  those accounts and characters yourself.
- **The live `worldserver.conf` and `playerbots.conf`.** Use the `.dist` files, which
  document every Centurion key.

The DLL can attest to the server (`ClientTweaks.Attest.*`, off by default). Its shared
secret is compiled into the DLL and is public in the clientedits source. If you turn
attestation on, rebuild the DLL with your own secret and set the same value in
`worldserver.conf`.
