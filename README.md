# GT6HACKFIX

**Static recompilation of Gran Turismo 6 (PS3, title ID `NPUA81049`) into a native PC executable — no PS3 emulator involved.**

This is a private fork of [sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp), a general-purpose PS3 static-recompilation toolkit. Instead of interpreting PowerPC/SPU instructions at runtime the way an emulator (e.g. RPCS3) does, `ps3recomp` lifts the game's PPU/SPU code ahead of time into C/C++ and compiles it with a normal compiler. This repository is the GT6-specific application of that toolkit: the lifted game code, the HLE (high-level emulation) bridges for the PS3 system calls GT6 actually calls, and the SPURS/PDI runtime work needed to get the title running.

## Status

**Boots, does not yet render or reach gameplay.** This is genuinely hard, ongoing reverse-engineering work — see [`historico_ia.txt`](historico_ia.txt) for the authoritative, session-by-session log (what was tried, what was proven, and the exact point to resume). As of the last confirmed session:

- The recompiled `EMAIN.ELF` (GT6's real main executable, 69,453 lifted PPU functions) loads, sets up TLS, dispatches to the entry point, and creates PPU worker threads.
- It reaches the D3D12 backend, sets up render targets/shaders, and issues `CLEAR_SURFACE`/`DRAW_IMMEDIATE` — but never a real draw call, so every captured frame is still a flat background color.
- SPURS/PDI (the PS3's cooperative SPU scheduler, which GT6 uses to stream its `PDIPFS` game data) real workloads (WIDs 5/6/7) get scheduled and run DMA against real `PDIPFS` files, but the specific completion callback that should hand control to the game's own UI/menu code (`func_00A70974` → `func_00A710FC`, reached through a vtable slot, not a direct call) is never invoked.
- Root cause of that last gap is still open. Static analysis alone (reading the lifted/disassembled code without running it) hasn't been enough to pin it down — see the bottom of `historico_ia.txt` for what was ruled out and what's still untested.

Building and running the game harness currently requires **Windows + MSVC + a D3D12-capable GPU** (see below). The GT6-specific harness code (`gt6/main.cpp`) calls the D3D12 backend directly; porting it to also build headless on Linux (there's already an `rsx_null_backend` upstream that could stand in for D3D12) is a known, not-yet-done next step.

## Repository layout

- [`gt6/`](gt6/README.md) — the GT6-specific worktree: HLE bridges (`gt6_hle.cpp`), the native runner (`main.cpp`), SPU job/policy images, and the `emain_project/` CMake target that builds `GT6MainRecomp.exe`.
- [`historico_ia.txt`](historico_ia.txt) — the project's memory. Read this before touching anything; it has the full history, closed decisions, measurements, and mistakes already made.
- `runtime/` — game-agnostic PPU/SPU emulation core (context, memory, syscalls, lifecycle).
- `libs/` — HLE implementations of the PS3 system libraries GT6 calls (`cellFs`, `cellSpurs`, `cellGcmSys`, audio, input, etc.).
- `tools/` — the Python lifter/disassembler/analysis pipeline (`ppu_lifter.py`, `spu_lifter.py`, `show_func.py`, and friends) that turns the decrypted ELF into the C++ under `gt6/`.
- `docs/` — the upstream `ps3recomp` framework documentation (architecture, build system, platform abstraction, NID system, etc.) — not GT6-specific, but explains the machinery GT6's port is built on.

## Building

### Windows (primary target — needed for actual gameplay testing)

Visual Studio 2022 (MSVC) + CMake + Ninja, per [`docs/BUILDING.md`](docs/BUILDING.md):

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4

cd gt6\emain_project
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
```

Run against your own **legally decrypted** `EMAIN.ELF` (see [`gt6/emain_project/README.md`](gt6/emain_project/README.md) for where the runner expects the PDIPFS-mounted game data):

```powershell
.\build\GT6MainRecomp.exe "path\to\EMAIN.ELF"
```

### Linux (runtime library only, experimental)

The game-agnostic `ps3recomp_runtime` library builds clean natively with GCC/Clang — verified with GCC 16 on Arch:

```bash
cmake -B build -G Ninja
cmake --build build
```

The GT6 game harness (`gt6/emain_project`) does **not** build on Linux yet — it needs the D3D12 → `rsx_null_backend` swap and a couple more `_WIN32`-guard fixes described in `historico_ia.txt`'s most recent entry.

## Game files

**No game data, ROM/ISO images, decrypted executables, or PS3 firmware modules are included in this repository.** You need your own legally owned copy of Gran Turismo 6 and must decrypt/extract `EMAIN.ELF` and the `PDIPFS` data yourself. `.gitignore` deliberately excludes `*.elf`, `*.self`, `*.bin`, `*.pkg`, and similar extensions for this reason.

## Credits

Built on [ps3recomp](https://github.com/sp00nznet/ps3recomp) by Ned Heller ([sp00nznet](https://github.com/sp00nznet)) and contributors — see [`CONTRIBUTORS.md`](CONTRIBUTORS.md). MIT licensed, see [`LICENSE`](LICENSE). The upstream framework's own documentation (architecture, NID system, platform abstraction, module status) lives under [`docs/`](docs/) and still applies to this fork.
