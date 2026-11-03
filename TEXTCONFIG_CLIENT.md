# Reversible client text configuration

`tools/textconfig_client.ps1` stages the 18 extracted original INI files for a client launch with
`-textconfig`. It preserves their bytes except for two changes:

- `DefaultX5Game.ini`: `[Server] BootstrapUrl=http://127.0.0.1:8080/`.
- `DefaultEngine.ini`: `[SSL] DebuggingCertificatePath` points to the existing emulator server's
  public PEM certificate.

It also adds that certificate path to the user's existing
`%LOCALAPPDATA%\X5\Saved\Config\Windows\Engine.ini`, preserving unrelated settings. GeoIP,
certificate verification settings, global Windows trust and the server's certificate/key remain
unchanged. `BinaryConfig.ini` is copied byte-for-byte; `-textconfig` selects the text configuration
startup path discovered in this client build.

Run from `Emulator/`:

```powershell
# Read-only preflight works while the game is running.
.\tools\textconfig_client.ps1 -Action Configure -DryRun

# Close the game before applying or restoring configuration.
.\tools\textconfig_client.ps1 -Action Configure

# Launch through Steam with the emulator running.
& 'C:\Program Files (x86)\Steam\steam.exe' -applaunch 5280930 -textconfig

# Close the game again before restoring.
.\tools\textconfig_client.ps1 -Action Restore -DryRun
.\tools\textconfig_client.ps1 -Action Restore
```

The default game directory is
`E:\SteamLibrary\steamapps\common\ArcheAge Chronicles Demo`. `-GameDirectory`, `-Port` and
`-CertificatePath` can select another installation, loopback port or public PEM file. The
certificate file must already exist and must not contain a private key. The default is
`Emulator/out/native-server/localhost.pem`, which must match the certificate already loaded by the
running server. The helper never starts or restarts the server or game.

Original files and SHA-256 hashes are saved under ignored
`out/native-client/textconfig-backup/<backup-set>/`, with an active `manifest.json` in the parent
directory. All destination paths, original backups and staged hashes are checked before Restore
changes its first target. Manual edits cause restoration to stop for review. Interrupted staging
or restoration can resume when every target still matches either its recorded original or its
staged output. Existing directories are retained; only newly staged files are removed.

The observed client launch removed the redundant saved `Engine.ini`. If that is the only changed
target, close the client and use `-Action Restore -AllowMissingSavedEngine` (or add `-DryRun`
first). This explicit option allows restoration of its backed-up original only while the saved
file remains absent. Modified existing files still stop restoration.

This backup is a separate layer above `native_client.ps1`. Restore the textconfig layer first.
That returns the older helper's modified `DefaultX5Game.ini` byte-for-byte, after which the older
helper can restore its own original file or original absence.

The helper stages a supported game-local certificate loading path. It does not by itself prove
the original client has accepted that certificate or completed its world handshake; consult
`VALIDATION.md` for runtime observations. The completed textconfig trial still displayed official
regions despite Steam passing the flag. This workflow has not established a persistent redirect.
