# Steam API compatibility for the researched client

The researched `v26.10.18.734` Windows client imports Steamworks v161. A direct
launch with its original `steam_api64.dll` fails when Steam cannot initialize.
The local test uses a dynamically linked [Goldberg Steam Emulator](https://gitlab.com/Mr_Goldberg/goldberg_emulator)
build to satisfy those client imports without starting Steam. The Project Chronicles account
grant is still issued and checked by the emulator server. The compatibility
ticket only lets the unmodified client complete its Steam API callback; it is
not accepted as account authority by the server.

`goldberg-v161.patch` applies to upstream commit
`475342f0d8b2bd7eb0d93bd7cfdd61e3ae7cda24`. It adds the v161 internal
initialization exports, implements the `GetAuthTicketForWebApi` callback used by
this client, and adapts the upstream build to the pinned vcpkg Protobuf/Abseil
libraries. It contains no diagnostic network probe or local log path.

From `Emulator/`, after configuring the pinned vcpkg dependencies:

```powershell
pwsh -NoProfile -File tools/build_steam_compat.ps1 -VcpkgRoot D:\Programas\vcpkg
```

The script produces a new ignored `out/steam-compat-*` folder. Its `payload/`
tree overlays the matching game directory. It includes `steam_api64.dll` in
the client's Steamworks v161 directory; the two required runtime DLLs beside
both that DLL and the game executable; the app ID and disabled Steam networking
and overlay settings; the LGPL license; and the upstream source archive plus
our patch. Do not publish the diagnostic `out/vendor/goldberg_emulator` checkout
or a DLL copied from an earlier trial. A signed game release must include this
payload and pin its hashes. The launcher should only launch the verified game
release, so players are asked for at most the game folder.

The isolated local client showed the NA realm and existing character selection
with the clean Release payload and no Steam process. With the separate guarded
user-mode world helper, it then completed the local world TLS connection and
rendered the world HUD. The helper restored its temporary client-memory changes.
This observation applies to the researched build and loopback server. Hosted
world entry and signed-release delivery remain to be verified before shipping.
In a fresh launcher-driven trial, `steam.exe` was absent from launch through the
playable overworld; the launcher armed the user-mode helper automatically. A PC
without the Steam installation or its Windows service has not been tested.
Steamless and RUNEAutoCracker were not used: this client reached the realm
without removing SteamStub from its executable.
