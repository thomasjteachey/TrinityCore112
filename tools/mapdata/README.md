# Server terrain data (maps / vmaps / mmaps) regeneration

`regen_map_data.ps1` drives the four TrinityCore tools from `Build\bin\RelWithDebInfo`
against the local client and writes into one work directory
(`C:\Projects\Gamedev\wow\data\mapdata` by default):

| step | tool | output |
|---|---|---|
| maps  | `mapextractor -i <client> -o <work> -e 1 [-m ids]` | `<work>\maps\<id><yy><xx>.map` |
| vmaps | `vmap4extractor -d <client>\Data\ [-m ids]` then `vmap4assembler Buildings vmaps [-m ids]` | `<work>\vmaps\<id>.vmtree`, `<id>_<xx>_<yy>.vmtile`, `<model>.vmo` |
| mmaps | `mmaps_generator [ids]` | `<work>\mmaps\<id>.mmap`, `<id><xx><yy>.mmtile` |

(`dbc\LiquidType.dbc` is extracted once into the work dir because `mmaps_generator`
needs it next to `maps\`/`vmaps\`.)

## The map list is optional

```powershell
.\regen_map_data.ps1                      # every map - identical to extractor.bat option 4 (hours)
.\regen_map_data.ps1 -Maps 1608           # just the Violet Hold BG map
.\regen_map_data.ps1 -Maps 1608,1620 -Deploy
```

The same is true of each tool on its own; the flag was added to all four and
omitting it keeps the old all-maps behaviour:

```
mapextractor    -m 1608          or  -m 1608,1620
vmap4extractor  -m 1608,1620     (models are extracted on demand from the maps' WDT/ADT
                                  placements; the global gameobject model list is skipped
                                  unless -g is given)
vmap4assembler  Buildings vmaps -m 1608,1620   (only those maps' trees + the models they spawn;
                                                gameobject model list left alone)
mmaps_generator 1608,1620        (one id or a list; --tile needs exactly one)
```

## Deploy

`-Deploy` copies the files this run produced (the named maps' tiles plus every model the
assembler wrote) to `~/mapdata-staging/<ts>/` on the game server and installs them into
each `-DeployTargets` data tree (dev + prod by default). Anything overwritten is first
copied to `<data>/backup-regen-<ts>/`. Files are written under a temp name and renamed,
so a running worldserver never sees a half-written file; instance maps pick the new data
up on their next load.

Notes:
* `mmaps_generator` normally *resumes* (skips tiles whose `.mmtile` already exists). A regen
  must not keep stale tiles, so the wrapper deletes the requested maps' mmap files first;
  `-Resume` keeps the tool's native behaviour.
* Model files for WMO doodads that were `.mdx` in the client come out as bare `<name>.m2`
  (no `.vmo`): the extractor writes the pre-rename name length, the name carries a trailing
  NUL, `fopen` truncates it - and the server truncates the same way when loading, so the
  pair is self-consistent. The live server already holds these files in exactly that form.
* Spawn IDs inside `.vmtile` files are per-run counters, so a per-map regen produces
  different (but internally consistent) IDs than a full run. Everything else - `.map`,
  `.vmtree`, models, `.mmap`, `.mmtile` - reproduced the live server's Violet Hold data
  byte for byte on 2026-08-15 (only the WMO's flag dword differs, by design).

## Violet Hold gotcha (2026-08-15)

The extractors read the client's highest-priority copy of every asset. The Violet Hold
override in `patch-Y.MPQ` was first built from `lichking.MPQ`'s DalaranPrison (3.0-era,
23527 tris) instead of `patch.MPQ`'s (3.3.5, 24109 tris incl. 582 extra collision faces).
A regen from that client produced a smaller vmap than the server had. The override is now
built with `tools\violet_hold\vhr_outdoors.py build-client-wmo-files <client>\Data\patch.MPQ <dir>`
and installed with `install-client-wmo`; the server data was regenerated from that.
