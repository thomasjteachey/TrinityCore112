# Running CenturionLauncher on Linux / Steam Deck (Proton)

The launcher is a Windows Electron app. There is no native Linux build — on Linux it
runs inside the same Proton/Wine prefix as the game.

## Setup

1. Extract `CenturionLauncher.exe` into the same directory as `WoW.exe`.
2. In Steam, **Add a Non-Steam Game** → browse to `CenturionLauncher.exe`.
3. Open the shortcut's **Properties**:
   - **Start In**: the WoW client directory (the folder containing `WoW.exe`).
   - **Compatibility**: check *Force the use of a specific Steam Play compatibility
     tool* and pick **Proton 9.0** or newer (Proton Experimental and Proton-GE 9/10
     also work).
4. Launch the shortcut. The launcher patches and starts `WoW.exe` inside the same
   prefix, so the game inherits the working Proton setup.

## Why a plain build hangs

Chromium — which Electron is built on — starts separate sandboxed child processes for
the renderer and the GPU. Wine does not implement the Windows sandbox APIs or the
D3D paths Chromium's GPU process expects, so those children die on spawn. The result
is a process that Steam shows as *running* (the **STOP** button appears, playtime
ticks up) while no window ever appears.

The launcher now detects Wine/Proton at startup and applies the compatibility
switches itself before Chromium initializes:

| Switch | Purpose |
| --- | --- |
| `--no-sandbox` | Wine has no Windows process sandbox |
| `--disable-gpu-sandbox` | same, for the GPU process |
| `--in-process-gpu` | no separate GPU process to crash |
| `--disable-gpu-compositing` | software compositing only |
| `disableHardwareAcceleration()` | software rendering instead of D3D/ANGLE |

Detection looks for Wine-only files in `System32` (`winemenubuilder.exe` and
friends), Wine/Proton environment variables, and the `HKCU\Software\Wine` registry
key. On real Windows none of these match and the app runs with full GPU
acceleration as before.

## Manual overrides

If detection is ever wrong, force it either way without rebuilding:

- Steam launch options — append the flag after `%command%`:

```bash
%command% --compat-mode
```

- Or set an environment variable (`1`/`true` to force on, `0`/`false` to force off):

```bash
CENTURION_COMPAT_MODE=1 %command%
```

## If it still does not start

1. Check `.launcher/runtime.json` next to `CenturionLauncher.exe`. It is written on
   every startup, before the window appears, and records the detected compatibility
   layer, Electron version, and whether the switches were applied. No file at all
   means the process died before Electron initialized — that is a Proton/prefix
   problem, not a launcher one.
2. Check `.launcher/log-*.txt` for `Child process gone`, `Renderer process gone`, or
   `Renderer failed to load` entries.
3. Capture a Proton log by adding `PROTON_LOG=1 %command%` to the launch options; the
   log lands in `$HOME/steam-<appid>.log`.
4. Try a different Proton. Proton 9.0+ is the baseline; Proton-GE tends to be the most
   forgiving. Very old Proton (5.x/6.x) will not run modern Chromium.

Please publish the Proton versions verified with each launcher release.
