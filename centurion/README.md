# centurion/

Everything the Centurion server runs on, apart from its source code. How to set up a
realm from it is in the [repository README](../README.md#setting-up-your-own-centurion).

Snapshot taken 2026-09-26 from the live realm.

| folder | what |
|---|---|
| `sql/` | the auth, world and characters databases, with the bots, and `import.sh` to load them |
| `dbc/` | the server's 246 DBC files |
| `patches/` | every client patch the launcher installs, with `join.sh` / `join.ps1` for the ones stored in pieces |
| `conf/` | the live `playerbots.conf` and `AutoBalance.conf` |
| `client/dinput8.dll` | the prebuilt client-tweaks DLL, version 1.00017 |
| `launcher/` | CenturionLauncher 1.1.23 source, from [thomasjteachey/centurionlauncher](https://github.com/thomasjteachey/centurionlauncher) at `1a92391` |
