# Project Chronicles Launcher — Windows x64

Run `chronicles-launcher.exe` with the adjacent `web/` folder and all bundled runtime DLLs.
The supplied profile selects `http://127.0.0.1:8080` and world `127.0.0.1:8081`.
Start the local server separately. Hosted destinations and signing keys require an operator
profile; see `docs/project-configuration.md` in the source tree.
The interface, artwork, icons and fonts are bundled and work offline. Windows needs
the Microsoft Edge WebView2 Runtime and the Microsoft Visual C++ x64 runtime used by the build.
Inter and Cinzel fonts are bundled under `web/assets/fonts/` with their license notices; keep
that directory in the package. Font rendering does not depend on an external font service.

News articles come from the configured backend's PostgreSQL database. Home displays the latest
four published articles; News displays up to 40 and offers **Refresh news**. The launcher checks
automatically every five minutes. During an outage, articles already fetched in this launcher
session remain readable with a warning. A new session needs the backend to load its news.
Operators publish, update, or withdraw articles with the backend's `chronicles-news.exe` tool;
player launchers have no news editing credentials.

This package supplies the player-facing WebView2 application. With an adjacent
`launcher-config.json`, it connects to the pinned API and verifies signed game releases before
installing updates. The gold strip reports downloaded bytes and installation state.
World service “Configured” is configuration,
not a successful original-client world-entry trial.

Store displays the backend's published catalog with search/category filters and opens product
links in your default browser. Checkout takes place at the shop. Community similarly opens the
realm's published Discord, forum, website and support destinations. Operators manage these
records with `chronicles-content.exe`; player packages contain no publishing credentials.
Characters shows the signed-in account's saved roster, levels, classes and equipped slots.
Browsing these details leaves the game's selected world/character unchanged. Use **Open character
selection** to create or select characters in the game. Signing out clears the private roster.

Remote APIs require HTTPS; HTTP is accepted only on loopback. The current backend configuration
is local, with explicit hostname placeholders for later deployment. See
`docs/distribution-backend.md` in the source tree for release publishing and backend setup.

## Existing development environment

Point a packaged launcher at the configured Emulator directory:

```powershell
.\chronicles-launcher.exe --project-root 'D:\PROJETOS\Archeage Emulator\Emulator'
```

Normal builds under `out/build/windows/bin` discover that development root
automatically. A relocated executable instead uses its adjacent `runtime/` folder, unless
`--project-root` is supplied. The interface always loads from the executable's adjacent `web/`.
Keep an extracted package in a writable directory: runtime settings and logs are under
`<project-root>/out/launcher/`. The package contains no personal settings, certificates,
launch grants, accounts, game files, captures, or kernel driver.
With a distribution profile, the launcher selects the registered Steam game first, or another existing
Project Chronicles or ArcheAge installation in known local locations if Steam is unavailable. A player-selected game folder
takes precedence; if no game is found, the launcher prepares `runtime/game/` beside itself.
Project Chronicles verifies and updates files in the selected folder, including the Steam game when selected.
Players can change only the game folder in Settings.
Account creation and sign-in remain in the separate account dialog. Developer settings, memory
backend selection, local server controls and diagnostic views are hidden. The native UI action
also rejects changes to those settings, so hiding a field is not the only guard.

A distribution profile pins `server_url`, `patch_public_key` and `patch_channel`. These values
override player settings. The backend must already contain a signed release of the matching
client. Players may select a different dedicated game directory if needed.
Game files and operator signing keys are not included in this launcher ZIP.
The profile starts the executable inside that verified managed directory with a child-only
Steam application environment. For the researched `v26.10.18.734` build, a separately built
Steam API compatibility payload lets the client initialize without a running Steam process.
Include the payload described in `launcher/steam_compat/README.md` in the signed game release;
the launcher ZIP alone does not contain game files or that payload. Other client builds need
their own compatibility validation.

Distribution startup uses signed bootstrap templates and native C++ substitution of a fresh
account grant; players do not need Python, extracted config sources, OpenSSL CLI or the PlaySharp
bridge. A statically linked Windows user-mode memory library supports guarded reads, writes and
scans for the selected game executable, without packaging a bridge, MCP service or kernel driver.
Normal HTTPS validation remains enabled. In a fresh local launcher trial with `steam.exe`
absent, the researched original client reached a playable world and the launcher automatically
armed the guarded user-mode helper. It restored both temporary changes after the connection.
Distribution profiles still disable local world helpers, and the original
client's hosted world startup requires a separate compatibility trial. The local result does
not establish a shipped original-client launch.
The legacy development profile retains its existing Python/bootstrap/PlaySharp prerequisites.

## Interface

- Home: live lobby availability, selected region, client readiness and Play.
- Create account: open the account menu, then **New to Project Chronicles? Create account**. Players choose
  a unique account name (3–32 ASCII letters, digits or underscores) and a password (8–256 UTF-8
  bytes), and confirm the password locally. After creation, the launcher returns to sign-in with
  the new name filled in. Registration never saves the password or starts a game. Sign-in offers a single saved-login choice. Duplicate names, unavailable realms and rate limits
  show actionable errors. The configured backend must include `/emulator/v1/register`.
- Account menu: sign in before installing the game. Successful login shows your account and
  session status; failures remain in the dialog with retry feedback. **Keep me signed in on this
  PC** is selected by default. For password sign-in it saves the account, Project Chronicles session, and
  password in Windows Credential Manager for the current Windows user. Discord sign-in saves the
  Project Chronicles session, not the Discord access token. The saved login is independent of the launcher
  settings files and is kept separately for each realm. A previous encrypted `login.dat` is
  migrated after the Windows credential has been saved and read back, then the old file is
  removed. Closing clears in-memory secrets.
  A saved password can renew an expired session; connection failures retry at most once per
  30 seconds. A rejected saved password clears the saved login and requires manual sign-in.
  A saved Discord session requires fresh authorization after it expires. **Forget saved login**
  removes saved credentials while preserving an active session; **Sign out** removes the current
  realm's credentials and revokes its launcher session. Switching realms retains each realm's
  saved credentials. An unreadable saved record fails closed with a visible recovery message.
- Play: reuse the current account session to request a fresh one-use launch grant. Starting a game
  preserves the launcher login. Sign out revokes the launcher session without revoking an already
  running game's session. An unavailable realm is shown separately from an expired login.
- News: bundled dispatch cards with full articles and contextual navigation.
- Characters: continue to the game's character selection; no invented character records.
- Store/community: explicit availability information until those services are configured.
- Settings: game folder and file-update actions in a distribution profile. Development builds
  retain local server, redirect, endpoint and helper controls in Diagnostics and Activity logs.

Closing or cancelling sign-in clears its password. Passwords and tokens are not stored by the UI.
The native host accepts bridge messages from its exact bundled page only and blocks navigation
and popups. When a client is detected, Play becomes a disabled **Game running** status in every
launch mode and the launcher moves to the Windows system tray. Signing in alone keeps Play
available. Restoring the launcher while playing keeps it open; Play returns after the client exits.
The minimize button also moves the launcher to the tray. Click its tray icon to restore it, or
right-click for **Show launcher** and **Exit launcher**. The close button exits the launcher.
If Windows cannot create the tray icon, the launcher minimizes to the taskbar instead.
Maximize/restore, close and the empty title-bar drag area are native actions.

## Build and package

From `Emulator/`:

```powershell
cmake --build --preset release --target chronicles-launcher chronicles-world-tls-local
pwsh -NoProfile -File tools/package_launcher.ps1 `
    -DistributionProfile out/distribution-backend/releases/launcher-config.json
```

The packaging script uses the CMake `Launcher` install component and writes a timestamped
directory and ZIP under ignored `out/distribution/`. It does not rebuild the runtime or include
local state. The generated profile contains only public configuration. Public deployment still
requires real API/world certificates and hostnames, a curated complete client release, and an
original-client world-entry test. Executable signing and an installer are separate release steps.
