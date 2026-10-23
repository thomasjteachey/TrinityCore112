# centurion/

Everything the Centurion server runs on, apart from its source code. How to set up a
realm from it is in the [repository README](../README.md#setting-up-your-own-centurion).

Snapshot taken 2026-10-23 from the live realm.

| folder | what |
|---|---|
| `sql/` | the auth, world and characters databases, with the bots, and `import.sh` to load them |
| `dbc/` | the server's 246 DBC files |
| `mmaps/` | Summoner's Rift's navmesh (map 1690), which cannot be generated from the client |
| `patches/` | every client patch the launcher installs, with `join.sh` / `join.ps1` for the ones stored in pieces |
| `conf/` | the live `worldserver.conf` (passwords, folders, SOAP, realm id and port reset to the `.dist` defaults), `playerbots.conf` and `AutoBalance.conf` |
| `client/dinput8.dll` | the prebuilt client-tweaks DLL, version 1.00027 |
| `launcher/` | CenturionLauncher 1.1.26 source, from [thomasjteachey/centurionlauncher](https://github.com/thomasjteachey/centurionlauncher) at `38cb772` |
