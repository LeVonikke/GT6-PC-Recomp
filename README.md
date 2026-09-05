# GT6 PC Recomp

![C++](https://img.shields.io/badge/C%2B%2B-blue) ![Status](https://img.shields.io/badge/status-active-brightgreen) ![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey) ![License](https://img.shields.io/badge/license-see%20LICENSE-informational)

**Static recompilation of Gran Turismo 6 (PS3, title `NPUA81049`) into a native PC executable — no PS3 emulator involved.**

Instead of interpreting PowerPC/SPU instructions at runtime the way an emulator (RPCS3) does, this project lifts GT6's actual PPU and SPU machine code ahead of time into plain C/C++ and compiles it with a normal compiler. The output is a real native executable that runs GT6's own code directly on the CPU — the same approach used by projects like Zelda64Recomp, N64: Recompiled, and the [ps3recomp](https://github.com/sp00nznet/ps3recomp) framework this is built on.

**If you're new here: read [`historico_ia.txt`](historico_ia.txt) before touching anything.** It's the project's real memory — a dated, append-only log of every session, every hypothesis tried, every bug found and fixed, and every dead end already ruled out. This README is an entry point and orientation; that file is the actual source of truth, and it is *long* (hundreds of dated entries) for a good reason: this kind of reverse-engineering work is easy to accidentally re-investigate from scratch if the history isn't kept.

## Status, honestly

**Boots deep into real PS3 system emulation. Does not yet render a frame or reach gameplay.** Concretely, on a real run against the actual decrypted `EMAIN.elf`:

- The executable loads, sets up TLS, dispatches to the real entry point, and creates ~29 PPU worker threads matching the game's own boot sequence (peripheral detection, `cellMouse`/`cellKb` init and teardown, GCM/RSX setup, tile binding, `SetPrepareFlip`).
- SPURS/PDI (the PS3's cooperative SPU task scheduler, which GT6 uses to stream its `PDIPFS` game data) initializes for real, dispatches real workloads, and issues real DMA against real `PDIPFS` files on disk.
- A real, hardware-verified job-dispatch mechanism exists in the game's own code — at least two independent "worker pool" dispatch loops were found and confirmed live with `gdb` — but **the actual job/work pointer these loops are supposed to receive is never installed**. The loops run correctly, forever, doing nothing, because nobody ever hands them anything to do.
- Three real bugs have been found and fixed in *our own* runtime this way (not GT6's code): a genuine deadlock in the thread-join cleanup path, a missing `pthread_exit()` equivalent on Linux that let "finished" threads keep running, and a completely unimplemented LV2 syscall (`sys_mmapper_enable_page_fault_notification`) that GT6 depends on for on-demand memory streaming. Each fix measurably unblocked more of the boot sequence — but none of them, alone or together, has yet reached the actual root cause above.

**The root cause is still open.** It's the single most-searched-for thing in this project's history. See the most recent entries in `historico_ia.txt` for exactly what's been ruled out (with live `gdb` watchpoint evidence, not guesses) and the concrete next place to look — the current best lead is to trace one level further up the call graph, to whoever *allocates and initializes* the job-dispatch slot objects, since the missing piece has to be something that would affect multiple independent dispatch loops at once, not a bug local to any single one.

## It builds and boots on both Windows and Linux

This is worth stating plainly because past documentation (including in this repo, and in `ps3recomp` upstream) assumed Windows/MSVC/D3D12 as a hard requirement. That's no longer true. The runtime and the GT6 harness both build and run natively on Linux (verified on Arch, GCC), headless (no D3D12 dependency — non-Windows builds skip straight to the RSX-null path). Most of the deep debugging work in this project's recent history was done entirely on Linux, using `gdb` live against the running process — hardware watchpoints on guest memory, breakpoints inside the recompiled code, the works. That workflow is genuinely one of this project's strengths: because the game's code is *compiled*, not interpreted, a normal debugger sees real, named C++ functions and can set real breakpoints inside GT6's own recompiled logic. See `historico_ia.txt`'s recent entries for the actual technique (including the non-obvious `handle SIGSEGV nostop noprint pass` gdb setting needed, since this project relies on real SIGSEGVs for lazy guest-memory commit).

## Branches

- **`merge-upstream-fold`** (default) — the actively developed branch. Includes 542 commits merged in from upstream `ps3recomp` (safely, cherry-picked/reconciled rather than blindly merged — see `historico_ia.txt` around 2026-08-25 for the reasoning and what was deliberately *not* adopted, e.g. an independent SPURS rewrite with real endianness-regression risk), plus all of this project's own SPU/PPU lifter fixes, HLE work, and the live-debugging investigation described above. **Start here.**
- **`master`** — an older, more conservative snapshot, kept as a stable reference point from before the upstream merge. It does not have the three bug fixes or the deepest boot investigation described above.

## Repository layout

- [`gt6/`](gt6/README.md) — the GT6-specific application: HLE bridges (`gt6_hle.cpp`), the native runner (`main.cpp`), SPU job/policy images, the `emain_project/` CMake target that builds `GT6MainRecompHook`, and `emain_recompiled/` (gitignored — the lifter's actual C++ output; regenerate it yourself from your own game copy, see below).
- [`historico_ia.txt`](historico_ia.txt) — read this first. The project's full history, closed decisions, measurements, live-debugging findings, and mistakes already made and fixed. Dated, append-only, in Portuguese.
- `runtime/` — game-agnostic PPU/SPU emulation core: guest memory, LV2 syscalls, thread lifecycle, the PPU/SPU execution context.
- `libs/` — HLE implementations of the PS3 system libraries GT6 calls (`cellFs`, `cellSpurs`, `cellGcmSys`, audio, input, font, codec, etc.).
- `tools/` — the Python lifter/disassembler/analysis pipeline (`ppu_lifter.py`, `spu_lifter.py`, `find_functions.py`, `elf_symbols.py`, and others) that turns a decrypted PS3 ELF into the C++ under `gt6/emain_recompiled/`.
- `docs/` — the upstream `ps3recomp` framework's own documentation (architecture, build system, NID system, platform abstraction) — not GT6-specific, but explains the machinery this port is built on.

## Building

Both platforms build the same two-step way: the game-agnostic runtime library first, then the GT6-specific harness against it.

### Linux

```bash
# 1. Runtime library
cmake -B build-linux -G Ninja
cmake --build build-linux

# 2. GT6 harness
cd gt6/emain_project
cmake -B build-linux -G Ninja
cmake --build build-linux --target GT6MainRecompHook
```

**Memory warning, seriously:** the lifter's generated C++ chunks (`gt6/emain_recompiled/ppu_recomp_0XX.cpp`) are enormous — each one can use 4-5GB of RAM to compile even at `-O0`. On a machine with 16GB or less, **never build with high `-j` parallelism** for these specific files; `-j1` or `-j2` at most, or you will OOM the machine (this has happened, more than once, documented in `historico_ia.txt`).

### Windows

MSVC + CMake + Ninja, same two-step structure:

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2

cd gt6\emain_project
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
```

Same memory warning applies.

## Running

```bash
./gt6/emain_project/build-linux/GT6MainRecompHook /path/to/your/decrypted/EMAIN.elf
```

A handful of environment variables gate specific, still-experimental code paths (all opt-in, all default off/safe):

| Variable | Effect |
|---|---|
| `GT6_RPCS3_STORAGE` | Root of your RPCS3-style storage tree (where `dev_hdd0/game/NPUA81049/USRDIR` lives). Defaults to a stale hardcoded path — always set this explicitly. |
| `GT6_PDI_REAL_KERNEL1` | Enables the real SPURS Kernel1 bridge instead of the earlier HLE shortcut. Needed to get past early boot. |
| `GT6_PDI_KERNEL1_RECONCILE_FLAG`, `GT6_PDI_KERNEL1_DISPATCH_ABI` | Companion fixes for the same bridge (dispatch ABI r4/r5 reconstruction). Used together with the above in every recent working session. |
| `GT6_SMOKE_TIMEOUT_MS` | Passive watchdog: dumps a thread snapshot after N ms and exits (Windows) or just logs (Linux). Doesn't change execution otherwise. |
| `YDKJ_HANDLERTRACE` | Verbose trace of the VBlank/Flip handler dispatch path. |

There are many more `GT6_*`/`YDKJ_*` flags scattered through the codebase gating specific diagnostic probes added during investigation — grep for `getenv("GT6_` / `getenv("YDKJ_` if you need one, and check `historico_ia.txt` for what a given session was using at the time.

## Game files — not included, and never will be

**No game data, disc images, decrypted executables, save data, or PS3 firmware modules are included in this repository**, and `fw_spu/*.prx` is `.gitignore`d specifically because early in this project's history real Sony PS3 system firmware modules were accidentally committed — they've since been scrubbed from the entire git history with `git-filter-repo`. You need:

1. Your own **legally owned** copy of Gran Turismo 6, decrypted yourself (`EMAIN.elf`, `EBOOT.BIN`, the `PDIPFS` data).
2. Your own copy of the specific PS3 SPU firmware modules referenced under `fw_spu/` (`libsre.prx` and friends) — same rule: get these from firmware you're licensed to use, drop them in `fw_spu/`, do not ask for or share copies.

## How to continue this

1. Read `historico_ia.txt`, at least the last 10-15 dated entries, to get the actual current state (this README is a snapshot; that file is live).
2. Build on Linux — it's the faster iteration loop and where the deepest debugging happened.
3. Run with the env vars above against your own game copy, attach `gdb` (`sudo gdb -p <pid>`, remembering `handle SIGSEGV nostop noprint pass` before continuing), and pick up the specific next-step pointer at the bottom of the most recent `historico_ia.txt` entry.
4. When you find something — confirm it live before writing it down. This project's history has a hard-won discipline of "verified in a live run, not guessed from reading code," and it's the reason genuinely wrong dead-end theories from months ago don't keep getting re-investigated. Keep it that way; append to `historico_ia.txt`, don't rewrite it.

## Credits

Built on [ps3recomp](https://github.com/sp00nznet/ps3recomp) by Ned Heller ([sp00nznet](https://github.com/sp00nznet)) and contributors — see [`CONTRIBUTORS.md`](CONTRIBUTORS.md). MIT licensed, see [`LICENSE`](LICENSE). The upstream framework's own documentation (architecture, NID system, platform abstraction, module status) lives under [`docs/`](docs/) and still applies to this fork.
