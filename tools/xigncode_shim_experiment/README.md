# Experimental local Xigncode export shim

This standalone Windows x64 project implements the **observed export ABI** of the original
`x3_x64.xem`. It has reached the game's title screen from a private client copy and allowed
two private client processes to run concurrently. It does not reproduce Xigncode's runtime,
and native login or world entry with this shim remains unverified. Do not place it in the
installed game directory.

The original module at `dumps/x3_x64.xem` has SHA-256
`76b5c96aa19142596a1d988b5c0322891721e1cc669edbbccebdfd913508fb4b`.
Its PE exports are ordinals 1, 2, 3, 4, and 99. Game code at `0x146536580` resolves
ordinals 1 and 4. Both take `RCX=void** output`, `EDX=uint32 method_id`, return a signed
32-bit status, and write a function pointer on success. The game currently resolves
method IDs 1, 2, 22, and 26 through ordinal 1, and ID 34 through ordinal 4. The
`x3_x64.xem` address/ABI evidence is in `dumps/x3_x64_analysis.md` and
`dumps/ida_research/xigncode-local/FINDINGS.md`.

The shim implements only those observed methods. Its ID 1 returns Boolean success;
ID 34 returns status zero; ID 22 ignores callback registration; ID 26 returns success
for observed commands 200, `0x55`, and `0x1BBC`; ID 2 returns zero. Unknown IDs and
commands fail closed. These responses are hypotheses for an isolated future trial.
In particular, no callback, child module, device communication, or runtime state is
emulated. A client may require any of those before or after world entry.

Build and test independently of the main emulator:

```powershell
. .\tools\dev_shell.ps1
cmake -S tools/xigncode_shim_experiment -B out/xigncode-shim -G "Ninja Multi-Config" -DCMAKE_CXX_COMPILER=cl.exe
cmake --build out/xigncode-shim --config Release
```

The build finalizes the DOS-header bootstrap marker automatically after linking.
The marker is computed per build: it combines the link timestamp with a section hash, and the
post-build step finalizes it. `0x8C105385` is only an example from one earlier build.
The two tests check the exports through `LoadLibrary`/`GetProcAddress` and independently
recalculate the marker. Neither launches the game or modifies the installed client.
The build output stays under the ignored `out/` directory.

The marker is required because the game's embedded bootstrap checks the selected
module **before** calling `LoadLibrary`. Its internal routine opens the PE file,
hashes each section's raw bytes in 4 KiB chunks, XORs the sum with the PE
`TimeDateStamp`, and compares the result with DOS-header words at `0x1C` and `0x1E`.
The installed original has stored and computed marker `0x82703FE2`. The first
unfinalized shim had stored marker zero and computed marker `0x8C105385`, which
explains the initial contained trial's lack of a loaded shim module. Passing this
checksum cleared that early gate in the private startup trial.
`verify_bootstrap_integrity.py` reproduces the check offline without altering inputs.

## Private-copy install and restore

`install_private_shim.ps1` accepts only a client tree prepared by
`tools/prepare_private_client.ps1`. It checks the `.local-client-copy.json` marker,
its source path, the private executable hash, the source module hash, and every private
ancestor of the target for reparse points. It writes only
`X5/Binaries/Win64/XIGNCODE/x3_x64.xem` and backup/state files under that private root.
The source game is read for comparison but never written. The helper refuses a stale
source, an unknown current module, a duplicate backup, or a shim outside this
emulator's `out/` directory.

From `Emulator/`, after the build and a completed private copy:

```powershell
pwsh -File tools/xigncode_shim_experiment/install_private_shim.ps1 `
    -Action Status -PrivateRoot 'E:\ArcheAgeChronicles-LocalTrial'
pwsh -File tools/xigncode_shim_experiment/install_private_shim.ps1 `
    -Action Install -PrivateRoot 'E:\ArcheAgeChronicles-LocalTrial'
pwsh -File tools/xigncode_shim_experiment/install_private_shim.ps1 `
    -Action Restore -PrivateRoot 'E:\ArcheAgeChronicles-LocalTrial'
```

The default expected source is `E:\SteamLibrary\steamapps\common\ArcheAge Chronicles Demo`.
Pass `-Source` explicitly for a different source and expect it to match the marker.
The helper copies the original private module to a backup, records both hashes,
then swaps the private module with `File.Replace`. Restore checks the shim hash,
replaces it with the byte-identical backup, and removes the state and backup only
after verification. If a replacement is interrupted, the state and backup remain
for inspection and `-Action Restore`. Do not run the client during a swap.
Install also runs the offline marker verifier and refuses an invalid shim.

`tools/launch_private_client.ps1` checks the private shim and outbound firewall rules before
starting the private executable. Each named account needs its own fresh bootstrap archive with
a one-use launch grant. The no-grant v4 development archive can test startup but receives HTTP
401 from a strict persistent server.
