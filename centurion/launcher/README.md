# centurion-launcher

An Electron application with React and TypeScript

## Recommended IDE Setup

- [VSCode](https://code.visualstudio.com/) + [ESLint](https://marketplace.visualstudio.com/items?itemName=dbaeumer.vscode-eslint) + [Prettier](https://marketplace.visualstudio.com/items?itemName=esbenp.prettier-vscode)

## Project Setup

### Install

```bash
$ npm install
```

### Development

```bash
$ npm run dev
```

### Production build

```bash
$ npm run dist
```

> The Windows build produces a standalone portable executable with automatic updates.

## Steam Deck and Linux through Proton

Steam Deck/Linux support uses the Windows portable launcher inside Proton or Wine. Put
`CenturionLauncher.exe` in the same directory as `WoW.exe`, add the launcher to Steam
as a non-Steam game, and force a Proton compatibility tool in the shortcut's
**Compatibility** settings. Set the shortcut's **Start In** directory to the same WoW
client directory.

The launcher starts `WoW.exe` inside the same compatibility environment. Launcher
self-updates are offered as a manual ZIP download under Proton/Wine; close the
launcher, extract `CenturionLauncher.exe`, and replace the existing executable. Game
patch downloads and verification continue to run in the launcher.

Wine and Proton are detected at startup and Chromium is switched to an unsandboxed,
software-rendered configuration, without which the launcher hangs with no window.
See [docs/linux-proton.md](docs/linux-proton.md) for the full setup, the manual
`--compat-mode` / `CENTURION_COMPAT_MODE` overrides, and troubleshooting steps.

For reliable support, publish the Proton versions tested with each launcher release.

## Launcher self-update hosting

See [docs/self-update.md](docs/self-update.md) for details on how to host the launcher
installer and generated update metadata so that in-app updates work correctly.
