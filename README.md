# GT6 PC Recomp

**Static recompilation of Gran Turismo 6 (PS3, title `NPUA81049`) into a native PC executable — no PS3 emulator involved.**

> **This is the `master` branch — an older, conservative snapshot.** Active development happens on **[`merge-upstream-fold`](../../tree/merge-upstream-fold)**, which is also this repository's default branch. It has three real bug fixes and a much deeper, `gdb`-verified boot investigation that `master` doesn't. If you're here to actually work on this project, switch to `merge-upstream-fold` and read its README instead — this one is kept mainly as a stable pre-upstream-merge reference point. See `historico_ia.txt` (present on both branches) around 2026-08-25 for why the branches split this way.

Instead of interpreting PowerPC/SPU instructions at runtime the way an emulator (RPCS3) does, this project lifts GT6's actual PPU and SPU machine code ahead of time into plain C/C++ and compiles it with a normal compiler — the same approach used by projects like Zelda64Recomp and the [ps3recomp](https://github.com/sp00nznet/ps3recomp) framework this is built on.

## Status (as of this branch)

**Boots, does not yet render or reach gameplay.** See [`historico_ia.txt`](historico_ia.txt) for the full, dated log of what's been tried and proven. This branch reaches D3D12 render-target/shader setup and issues `CLEAR_SURFACE`/`DRAW_IMMEDIATE`, but never a real draw call. `merge-upstream-fold` has gone considerably further past this point — see that branch for the current state of the investigation.

## Repository layout

- [`gt6/`](gt6/README.md) — the GT6-specific application: HLE bridges (`gt6_hle.cpp`), the native runner (`main.cpp`), SPU job/policy images, and the `emain_project/` CMake target that builds the game harness.
- [`historico_ia.txt`](historico_ia.txt) — the project's memory. Read this before touching anything.
- `runtime/` — game-agnostic PPU/SPU emulation core (context, memory, syscalls, lifecycle).
- `libs/` — HLE implementations of the PS3 system libraries GT6 calls (`cellFs`, `cellSpurs`, `cellGcmSys`, audio, input, etc.).
- `tools/` — the Python lifter/disassembler/analysis pipeline that turns a decrypted PS3 ELF into the C++ under `gt6/`.
- `docs/` — the upstream `ps3recomp` framework's own documentation.

## Building

### Windows

Visual Studio 2022 (MSVC) + CMake + Ninja:

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2

cd gt6\emain_project
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
```

**Memory warning:** the lifter's generated C++ chunks are enormous (4-5GB RAM to compile each, even at `-O0`). Never build them with high `-j` on a machine with 16GB or less — `-j1`/`-j2` max, or you will OOM the machine.

### Linux

The runtime library builds clean natively (GCC/Clang). The GT6 game harness on this specific branch has known `_WIN32`-only assumptions in `gt6/main.cpp` (D3D12 backend) that haven't been ported here — **this is one of the things already fixed on `merge-upstream-fold`**, which builds and runs headless on Linux end-to-end.

```bash
cmake -B build-linux -G Ninja
cmake --build build-linux
```

## Game files — not included

**No game data, disc images, decrypted executables, save data, or PS3 firmware modules are included in this repository.** You need your own legally owned copy of Gran Turismo 6, decrypted yourself, and your own copy of the PS3 SPU firmware modules referenced under `fw_spu/` (`.gitignore`d — get these from firmware you're licensed to use).

## Credits

Built on [ps3recomp](https://github.com/sp00nznet/ps3recomp) by Ned Heller ([sp00nznet](https://github.com/sp00nznet)) and contributors — see [`CONTRIBUTORS.md`](CONTRIBUTORS.md). MIT licensed, see [`LICENSE`](LICENSE).
