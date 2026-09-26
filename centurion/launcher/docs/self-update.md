# Launcher self-update hosting

The standalone launcher uses the same update-server setting as game patches. By default
it checks:

```text
https://centurionpvp.com/downloads/centurionlauncher.version
https://centurionpvp.com/downloads/centurionlauncher.zip
```

`centurionlauncher.version` contains only the published version number. The launcher
compares it with `.launcher/centurionlauncher.version`, which records the last server
version that was installed successfully. When the server number is newer, the launcher
downloads `centurionlauncher.zip`. The ZIP contains the standalone
`CenturionLauncher.exe`.

The executable's embedded package version does not need to change for an updater test.
You can increment only the server's `centurionlauncher.version` and continue serving the
same ZIP. After the replacement succeeds, the launcher records the new server number
locally and will not download it again.

When the player selects **Restart to update**, a hidden helper waits for the current
launcher process to exit, replaces the old portable executable, and reopens it. The
`.launcher` settings directory beside the executable is not replaced or removed.

## Publishing a release

1. Run `npm run dist` on Windows.
2. Upload the generated `dist/centurionlauncher.zip` to the downloads directory.
3. After the ZIP is in place, edit the server's `centurionlauncher.version` to the
   version number you want to publish. Changing this file announces the release to
   existing launchers.

The update server must serve both files directly over HTTPS. The generated ZIP must
contain `CenturionLauncher.exe` at its root. Code-signing the portable executable is
strongly recommended before public rollout.

LAN clients may configure the update server with a literal HTTPS address such as
`https://192.168.1.226/downloads/`. For direct-IP update requests, the launcher still
uses `centurionpvp.com` for TLS SNI and certificate verification, so the existing
public certificate remains valid without a hosts-file or local-DNS entry. Certificate
verification is not disabled.

## Proton and Wine

The PowerShell replacement helper is not used when the launcher detects Proton or
Wine. A newer launcher is shown as a manual ZIP download so the player can close the
launcher and replace `CenturionLauncher.exe`.

For a manual-update release, update the version in `package.json` to match the version
published in `centurionlauncher.version`. The Windows replacement helper records the
published server version automatically, but a manually replaced executable relies on
its embedded package version.
