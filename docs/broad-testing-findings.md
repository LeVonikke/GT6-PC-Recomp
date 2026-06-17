# Broad-testing findings (harness run, June 2026)

First run of `tools/harness/ps3_recomp_harness.py` across the PSN library and a
decrypted-ELF corpus. This is the "turn one-off runs into a compatibility
matrix" pass — the point is to find recurring patterns that tell us what to
harden next. (Full machine-readable output lives under `_harness/`, which is
git-ignored; this file is the human summary.)

## 1. PSN library catalog (filename pass, no extraction)

- **1,345 archives** under `Z:\Roms\PS3\PSN` (PSN_1: 672, PSN_2: 673), **~1.13 TB** total.
- **1,339 / 1,345** carry a parseable title-id in the filename (`… [NPxx-NNNNN]`).
- **Region:** EU (SCEE) 661 · US (SCEA) 635 · JP (SCEJ) 28 · Asia 6 · unknown 15.
- **Content class** (PSN prefix 4th letter, coarse):
  - PSN full game (`B`): **710**
  - Demo / minis (`Z`): 273
  - PS1 / minis (`J`): 112
  - App / utility (`A`): 142
  - Other / disc-linked / unknown: ~108

**Takeaway:** the realistic recomp target space here is the **~710 full PSN
games** plus a long tail of minis. Region is overwhelmingly EU/US, so PPU ABI +
the western SDK versions dominate — good for prioritization.

## 2. Decrypted-ELF recomp triage (the real pipeline)

Corpus: Tekken 6, Marvel Ultimate Alliance, Simpsons Arcade, Tokyo Jungle, flOw
(plus `tornado.self`). All are PS3 PPU executables.

| Binary | machine | image base | functions | `.opd` seeded | notes |
|---|---|---|---:|---:|---|
| scout/tekken6.elf | PPC64 | 0x00010000 | 36,166 | 18,234 | healthy `.opd` |
| scout/simpsons.elf | PPC64 | 0x00010000 | 56,606 | 24,150 | healthy `.opd` |
| scout/mua.elf | PPC64 | 0x00010000 | 28,088 | **1** | `.opd` not located |
| tokyojungle/EBOOT.elf | PPC64 | 0x00010000 | 11,231 | 3,816 | healthy `.opd` |
| simpsons/EBOOT.elf | PPC64 | 0x00010000 | 5,170 | 889 | lifted tier-5 OK |
| flow/EBOOT.elf | PPC64 | 0x00010000 | 31,008 | **1** | `.opd` not located |
| scout/tornado.self | — | — | — | — | **blocked: no klicensee** |

Tier-5 lift validated on the Simpsons EBOOT: **5,170 detected → 14,560 lifted**
functions (mid-function tail-entries + gap-resident targets expand the count),
**59 MB** of C in 2 chunks, ~137 s with parallel lifting (caner #11). This
exercised the whole freshly-merged lifter stack (VMX handlers #8 + parallel #11
+ gap-target WIP) on a real game with no crashes.

## 3. Things we learned (actionable)

1. **`.opd` seeding is a big win where the table is found** — Tekken 6, Simpsons
   Arcade, Tokyo Jungle all seed thousands of address-taken functions (caner
   #13 doing exactly its job). **But two binaries (Marvel UA, flOw) seed only 1**
   — the shape-detector isn't locating their `.opd` (section names stripped /
   non-standard layout / older dumps). *Next:* make `find_functions`' `.opd`
   shape-detection more robust, and emit a loud warning when 0–1 descriptors are
   found on a binary this large (it almost always means a missed table, not a
   table-less binary). This is the single highest-leverage correctness follow-up.
2. **Image base is uniformly `0x00010000`** across every retail PS3 EXEC sampled —
   far less variance than the 360 side's high-base titles. The toolkit can lean
   on that assumption (and flag anything that deviates, as the report does).
3. **Decryption coverage is the scaling bottleneck for PSN**, not analysis.
   `tornado.self` (and the entire NPDRM library) needs the per-title klicensee
   (RAP/rif) before `ps3sce` can produce an ELF. We have exactly one RAP
   (Tokyo Jungle). *Next:* wire `klics.txt` / a RAP store into the decrypt tier so
   the harness can fan out over titles we *do* have keys for.
4. **We can't yet build the 360 harness's killer feature — the top-NID-imports
   stub-prioritization list — for EXEC ELFs.** `elf_parser --imports` only
   resolves PRX imports; a linked EBOOT carries its imports in the
   `.sceStub.text` / lib.stub section. *Next:* add an import extractor that walks
   an EXEC's stub section, pulls the NID table, and resolves it through the
   (now-correct, post-#9) `nid_database`. Run across the corpus, that yields the
   "which kernel/PRX functions does every game need" ranking that drives stub
   work — the same way the 360 report does.

## 4. Suggested next steps (in priority order)

1. `.opd` detection robustness + a "suspiciously few descriptors" warning.
2. EXEC import-NID extractor → wire into the harness → cross-title stub-priority
   ranking.
3. RAP/klicensee store for the decrypt tier → fan the harness out over the PSN
   titles we have keys for (start with the minis: smallest, simplest, fastest).
4. Tier-3 build/boot tiers (compile the lifted C, attempt a boot) once a target
   game's runtime is far enough along — mirrors the 360 harness tiers 3–4.
