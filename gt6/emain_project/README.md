# GT6 main executable recompilation

This target recompiles the decrypted `EMAIN.ELF`, the real Gran Turismo 6 main
executable. Its generated sources live one directory above to make the launcher
and main-program worktrees independent.

Build from a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
```

Run the resulting executable against the decrypted input:

```powershell
.\build\GT6MainRecomp.exe "C:\Users\LeVon\Downloads\TrueAncestor_SELF_Resigner_v1.98\self\EMAIN.ELF"
```

For a bounded startup trace, set `GT6_SMOKE_TIMEOUT_MS` before running. The
runner then prints the last firmware calls and exits cleanly at that deadline:

```powershell
$env:GT6_SMOKE_TIMEOUT_MS = 10000
.\build\GT6MainRecomp.exe "C:\Users\LeVon\Downloads\TrueAncestor_SELF_Resigner_v1.98\self\EMAIN.ELF"
```

The lift contains 69,453 native PPU functions across ten compilation units.
Further work is required to finish GT6-specific HLE, VFS, SPU, and RSX support.

Smoke-test status: the native runner loads both EMAIN image segments, sets up
TLS, dispatches the entry point, starts PPU worker threads, and reaches
`_cellGcmInitBody`. The lifter resolves both in-text relative switch tables and
literal-address `bctr` dispatches, so local targets such as `0x00A7CEE4` are
kept inside their lifted function instead of being sent to the indirect-function
resolver. The private Fiber initialization NID is registered, and the runner
mounts `/dev_hdd0` and `/dev_bdvd/PS3_GAME` from the configured RPCS3 storage
with guest-aware CellFs adapters.

The GCM path now returns guest addresses from `cellGcmGetOffsetTable` instead
of leaking host pointers; this removed the first post-GCM access violation.
The startup adapters also cover the reached GCM/SPURS NIDs, game-data/cache
paths, and guest-address CellFs unlink calls. GT6 now proceeds through private
firmware-module loading, SPU initialization, and additional PPU worker
creation, and remains running for more than 30 seconds without a host access
violation. The SPURS bootstrap now has a guest-safe lifecycle: it records LV2
queue attachment, adopts the SDK-internal startup job chain, and posts its
completion through that queue in the observed SPURS event format. It also
handles the public job-chain attribute/create/run/guard/join/shutdown imports.
`sys_lwcond` now blocks cooperatively and responds to signals instead of
spinning at full speed.

The current observable state is still the PPU worker/event loop. The next
functional requirement is execution (or title-specific HLE) of the SPU job
payloads themselves: the bootstrap event is consumed, but there is no SPU
worker producing the later guard notifications and game-side effects yet. It
does not yet render or boot into the game.

The mounted title data is the digital `NPUA81049` install in RPCS3 storage. A
physical `BCUS98296` `GT.VOL` is intentionally not overlaid: it is base version
01.00, while the installed title declares APP_VER 01.05. Mixing the volume with
the executable/update would make its offset and metadata tables unreliable. No
game data is copied into this repository.

The runner reports `CELL_GAME_GAMETYPE_HDD` for this PSN build. This selects the
native PDIPFS path, verified by successful reads of `PDIPFS/K/4D` and the volume
table of contents at `PDIPFS/9/9F/RS`. A read-only working extraction of the
same NPUA 01.05 PDIPFS is available outside the repository at
`D:/GT6 DATA/NPUA81049_01.05` (34,014 files, 23.83 GB). It was produced with
GTToolsSharp 5.3.3:

```powershell
GTToolsSharp.exe unpack `
  --input "E:/Emulation/storage/rpcs3/dev_hdd0/game/NPUA81049/USRDIR/PDIPFS" `
  --output "D:/GT6 DATA/NPUA81049_01.05" --no-print
```

The next lifting-specific issue is a computed branch to an internal basic block
(`0x00FBCD98` inside `func_00FBC470`). It is not an unresolved firmware import;
the lifter needs to emit an entry for such non-function branch destinations.
