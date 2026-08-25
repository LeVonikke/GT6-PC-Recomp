# GT6 static recompilation worktree

This directory contains the completed static-recompilation pipeline for the
decrypted `EBOOT.elf` provided with NPUA81049:

- `analysis/` contains the ELF image, OPD/function, and import manifests.
- `recompiled/` contains the generated native C++ PPU code.
- `gen/ppu_hle_nids.cpp` registers HLE handlers compiled from ps3recomp.
- `GT6Recomp` is a native Windows runner built with CMake.

Build the runtime first from the repository root, then build this project:

```powershell
cmake -S .. -B ../build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../build --parallel 4
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
```

Run it against the decrypted launcher:

```powershell
.\build\GT6Recomp.exe "E:\Emulation\storage\rpcs3\dev_hdd0\game\NPUA81049\USRDIR\EBOOT.elf"
```

`EBOOT.elf` is only GT6's bootstrap program (190 OPD functions); it transfers
control to `EMAIN.SELF`, the 9 MB encrypted main executable. A full GT6 port
requires a legally decrypted `EMAIN.ELF`; then repeat `ppu_loader.py`,
`ppu_lifter.py`, and `gen_hle_nids.py` using that ELF as input.

The main executable is now available as a separate worktree at
`emain_project/`, with its analysis and 69,453-function lift in the sibling
`emain_*` directories.

The launcher build has been smoke-tested through ELF loading, TLS setup, entry
dispatch, and PPU-thread creation. It then reaches HLE/indirect-call gaps,
which is expected until the main executable and its game-specific bridges are
ported.
