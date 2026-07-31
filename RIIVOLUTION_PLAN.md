# Native Riivolution support in USB Loader GX — scope, architecture & phased plan

Status: scoping draft. Source surveyed: `usbloadergx/` @ HEAD (shallow clone).

## Implementation status

- **Phase 0 — DONE and wired.**
  - Module `source/riivo/`: `RiivoTypes.hpp` (model), `RiivoParser.{hpp,cpp}`
    (pugixml port of `parser.py`), `RiivoConfig.{hpp,cpp}` (resolver, `${param}`
    substitution, built-in params, `IsValidForGame`, selection serialize/apply,
    gprintf dumps). `source/riivo` added to Makefile `SOURCES`.
  - Storage: `GameCFG.RiivoPath` + `GameCFG.RiivoConfig` (per-game only, no global /
    no INHERIT) in `CGameSettings.{h,cpp}` (struct, `operator=`, Save, parser,
    SetDefault).
  - UI: `settings/menus/RiivoSM.{hpp,cpp}` — per-game page (XML picker under
    `<dev>:/riivolution` + one row per `<option>` cycling choices). Launched from a
    Wii-only "Riivolution" button on `GameSettingsMenu` (button + index-matched
    dispatch, gated identically so indices stay in sync).
  - Boot: `BootGame()` parses the selected XML, applies the saved selection, resolves,
    and (Phase 0) gprintf-dumps the resolved set. Hook point for the Phase 1 engine.
  - **Verified:** every changed/new TU compiles with devkitPPC (`-mrvl -mcpu=750`,
    project flags + full include set): `RiivoParser.cpp`/`RiivoConfig.cpp` → `.o`;
    `RiivoSM.cpp`, `CGameSettings.cpp`, `GameSettingsMenu.cpp`, `GameBooter.cpp` →
    `-fsyntax-only` clean. Host-side functional test passes (parse → macro clone →
    id-filter → selection round-trip → resolve → substitution).
  - **Full-build note:** a complete `make`/link was NOT achievable in this shell for
    reasons unrelated to these changes: (1) the working-copy path contains spaces,
    which the devkitPro recursive Makefile can't handle; (2) the installed devkitPPC
    is newer than the repo's pinned `devkitpro/devkitppc:20250527` and hits a
    `socklen_t`/`sockaddr_storage` conflict between bundled `portlibs` and installed
    libogc in untouched upstream network code. Build via the pinned Docker image
    (`docker build -o . .`) or a space-free checkout with the matching toolchain.
- **Phase 1 — DONE (memory engine).**
  - `source/riivo/RiivoMemory.{hpp,cpp}`: applies `<memory>` patches to the loaded
    game binary — **direct** (`offset|0x80000000`, optional `original` byte-check),
    **valuefile** (loads bytes from SD, path = device+root+valuefile), **search**
    (scan DOL sections at `align` stride for `original`, write value, first match),
    **ocarina** (scan for `value`, advance to `blr` 0x4E800020, write
    `((target-blr)&0x03FFFFFC)|0x48000000`).
  - `gamepatches.c`: added `RiivoGetDOL{Count,Dst,Len}` accessors over the loaded DOL
    section list; **moved `ClearDOLList()` out of `gamepatches()`** into `BootGame`
    so the memory engine can scan the sections after gamepatches (Dolphin's "memory
    patches last" ordering). `gamepatches()` is only called from `BootGame`, so this
    is safe; `PatchFix480p`/Wiimmfi still run after the clear exactly as before.
  - `BootGame()`: resolves once (persisted `riivoSet` + `riivoDevice`), then after
    `gamepatches()` calls `Riivo::ApplyMemoryPatches(riivoSet, riivoDevice)` and
    `ClearDOLList()`.
  - **Verified:** all TUs compile with devkitPPC (`RiivoMemory.cpp`→`.o`;
    `gamepatches.c`, `GameBooter.cpp` clean). Host test of the *real* engine passes:
    search replace + ocarina blr-hook (correct `b`-form + delta) on a section buffer.
    Direct writes target `0x80xxxxxx` so they're validated by target-compile + the
    Dolphin-identical implementation (can't exec on host).
- **Phase 2 — DONE (savegame redirect).**
  - `source/riivo/RiivoSave.{hpp,cpp}`: maps `<savegame external= clone=>` onto USB
    Loader GX's NAND emulation — points the NAND-emu base path at the mod's folder so
    the mod save is isolated from the vanilla save; `clone=true` copies the existing
    save (`title/00010000/<id-hex>`) in on first run via `CopyDirectory`.
  - `JoinPath` promoted from `RiivoMemory` to public `Riivo::JoinPath` (RiivoConfig),
    reused by both memory and save modules.
  - `BootGame()`: before `SetupNandEmu`, if a `<savegame>` patch is active and NAND
    emu is enabled, overrides `NandEmuPath` to the mod folder (backed by a persistent
    `std::string`).
  - **Verified:** compiles with devkitPPC (RiivoSave/RiivoConfig/RiivoMemory→`.o`,
    GameBooter clean). Host test passes 4/4: emu-off skip, first-run clone from/to the
    correct title paths, no re-clone when the mod save exists, no clone when clone=false.
  - **Constraints (documented):** requires NAND emulation enabled for the game (if off,
    logs and skips rather than force-enabling, to avoid risky forced-emu boots);
    handles the `title/00010000` save class (not the `00010004` special-case games);
    assumes the mod folder is on a usable emu device. These are safe to relax later.
- **Phase 3 — IN PROGRESS.** Decision: **in-RAM cIOS DI read-hook, NOT a custom
  cIOS fork** — no cIOS to flash (no brick risk), works with the user's existing d2x,
  ships inside the loader. Matches how the original Riivolution homebrew works.
  - **DONE + verified (loader-side foundation):**
    - `RiivoFst.{hpp,cpp}` — parses the game FST (big-endian safe, Wii offset<<2
      un-shift), `FindFile` (full path or bare-filename, case-insensitive),
      `ListFolder` (recursive/direct). Host test 12/12.
    - `RiivoFile.{hpp,cpp}` — `BuildRedirects`: resolves `<file>`/`<folder>` patches
      against the FST into `RedirectSpec{discOffset,length,fileOffset,external}`;
      handles sub-range offset/length, `create=true` → Phase-4 created list, folder
      name-matching. Plus `FsDirLister` (readdir-backed external enumeration). Host
      test 8/8. All modules compile for target.
  - **REMAINING — hardware milestone (cannot compile/host-verify here):**
    1. *Fraglist + registration (loader C, compilable, not verifiable):* for each
       `RedirectSpec`, stat the external file (fill length when 0) and build its
       physical-sector fragment list via `get_frag_list_for_file`, assemble the
       redirect table, and hand it to cIOS via a new ioctl (`WDVD_SetRedirect`,
       suggested DI cmd `0xF7`/`0xF8`, parallel to `SETFRAG 0xF9`).
    2. *IOS DI read-hook (ARM/IOS, hardware-only):* runtime-patch the live cIOS DI
       read handler to check the redirect table before normal frag translation; on a
       hit, read the external file's bytes into the (decrypted) output buffer instead
       of the disc. d2x-version-specific; needs on-console iteration. Deferred rather
       than shipped unverified.
  - **Timing constraint (discovered during design — important):** the redirect
    fraglists must be built with SD/USB **devices still mounted**, i.e. before
    `ShutDownDevices()` (which runs right after `BootPartition`). But the in-RAM FST
    (`*0x80000038`) only exists *after* the apploader runs inside `BootPartition`.
    Therefore the builder must read the **FST from the disc** in the early window
    (partition open, devices up — near `SetupDisc`), NOT from the RAM copy. Read
    boot.bin (partition offset 0x420: dol/fst offset+size, shifted) → `WDVD_Read` the
    FST → `RiivoFst::Parse` → `BuildRedirects` → build fraglists → register → then let
    `BootPartition`/`ShutDownDevices` proceed. This ordering is the integration spec
    for the hardware step (not yet wired, to avoid unverifiable disc-read/timing code).
  - Note on why not pure-SETFRAG: the disc data cIOS frag-maps is *encrypted*;
    external Riivolution files are plaintext, so they must be substituted at the
    post-decryption read level — hence a read-hook, not just extra fragments.
- Phase 4 (`<folder>` create=true + in-RAM FST rebuild): not started.

## 1. What the loader already does (the pieces we reuse)

### 1.1 The boot flow and the injection window

`GameBooter::BootGame()` (`source/usbloader/GameBooter.cpp:275`) is the whole boot
pipeline. The relevant tail (lines 676–771):

```
BootPartition()                 -> runs apploader, loads main.dol into RAM,
                                   FST now in RAM at *(u32*)0x80000038,
                                   returns AppEntrypoint          (GameBooter.cpp:680)
ShutDownDevices()
gamepatches(...)                -> pokes the in-RAM DOL + fixed addresses (:723)
load_handler(Hooktype,...)      -> installs Gecko code handler   (:736)
PatchFix480p / patch_error_codes / Wiimmfi
Disc_JumpToEntrypoint(...)      -> jumps into the game           (:771)
```

**This is exactly the Riivolution `<memory>` window**: DOL is in RAM at its final
link addresses, the game has not started, everything from `0x80000000` is directly
writable. `gamepatches()` is literally "poke RAM before boot." Our memory-patch
engine slots in right beside it (a `riivo_apply_memory_patches()` call after
`gamepatches`, before `load_handler` so an ocarina payload can coexist with the
Gecko handler, or after — see §4).

### 1.2 The memory-patch mechanism (reuse target for `<memory>`)

`gamepatches()` (`source/patches/gamepatches.c:102`) iterates a list of DOL
sections registered during apploader load:

- `RegisterDOL(dst, len)` (`gamepatches.c:75`) — the apploader calls this for every
  DOL segment it reads (`source/usbloader/apploader.c:73`). `dolList[]` holds
  `{dst, len}` for each segment at its **final load address**.
- Each patcher scans/edits those buffers. Pattern-scan example: `anti_002_fix`
  (`gamepatches.c:218`) does exactly a `<memory search=>` — memcmp a pattern across
  a DOL section, write on match.
- Direct absolute writes are already done too, e.g. `*(u32*)0x80003140 = ...`
  (`gamepatches.c:209`), `write32`/`mask32` in the Wii U aspect code
  (`GameBooter.cpp:363`). That is a `<memory offset= value=>` primitive.
- `find_safe_space(addr, len)` (`gamepatches.c:234`) already finds scratch space in
  the DOL — a place to drop an ocarina/branch payload.

So all four `<memory>` variants map onto primitives that already exist in this file.

### 1.3 The hook / code-handler mechanism (reuse target for `<memory ocarina=>`)

- `dogamehooks(hooktype, addr, len)` (`source/patches/patchcode.c:78`) finds a known
  engine hook site (VI/KPAD/GX/etc. signatures at `patchcode.c:63`) and patches a
  branch to the Gecko code handler.
- The code handler lives at a fixed low-mem region: `codelist = 0x800022A8`,
  `codelistend = 0x80003000` (`patchcode.c:46`). `patchhook()` (asm in
  `source/patches/patchhook.S`) writes the branch.
- `ocarina_load_code()` / `load_handler()` build the Gecko code list and install the
  handler. The branch-to-blr encoding I already ported in `dol/gecko.py` is the same
  transform used for `<memory ocarina=>` (find pattern → advance to next `blr` →
  branch to payload). We reuse `find_safe_space` for payload placement.

### 1.4 The DVD read path (the crux for `<file>`/`<folder>`)

Two distinct read stages — do not conflate them:

1. **DOL load (loader-side).** The apploader pulls DOL segments with `WDVD_Read`
   (`apploader.c:71`), which is `IOS_Ioctl(_di_fd, IOCTL_DI_READ, …)`
   (`source/usbloader/wdvd.c:279`). This runs in the loader, before the game starts.
2. **Game runtime reads (in-game).** Once the game is running, its own
   `DVDReadPrio`/`DVDRead` calls go to the DI device, which **cIOS intercepts**. The
   loader never sees these — they are serviced inside IOS/cIOS.

How cIOS knows where the "disc" really is: the loader hands cIOS a **fragment list**
mapping virtual disc offsets → physical USB/SD sectors, via
`WDVD_SetFragList` → `IOCTL_DI_SETFRAG` (`wdvd.c:402`, called from
`set_frag_list()` `source/usbloader/frag.c:270`). cIOS's DIP module translates each
`IOCTL_DI_READ(disc_offset)` through this table to a physical sector and reads it.

Critically, **`get_frag_list_for_file(fname, …)` (`frag.c:163`) already maps an
arbitrary SD/USB file to a physical-sector fragment list** (FAT via
`_FAT_get_fragments`, NTFS, WBFS). That is the exact primitive needed to point a
redirected FST entry at an external file.

The embedded `source/mload/modules/dip_plugin_249.c` etc. are **precompiled ARM
blobs** for the legacy Hermes cIOS — not editable source. Modern setups use **d2x
cIOS** (separate repo), whose DIP module owns the read path. True file redirection
lives there, not in these blobs.

## 2. The core asymmetry

| Riivolution patch | Applied where | Difficulty | Reuses |
|---|---|---|---|
| `<memory offset= value=>` | loader RAM, pre-boot | trivial | direct writes |
| `<memory valuefile=>` | loader RAM, pre-boot | easy | file load + memcpy |
| `<memory search=>` | loader RAM, pre-boot | easy | `anti_002_fix` scan |
| `<memory ocarina=>` | loader RAM, pre-boot | medium | `find_safe_space` + gecko branch |
| `<savegame>` | loader, save path setup | medium | NAND-emu / `SavePath` redirect |
| `<file>` (replace existing) | **cIOS DI read hook** | hard | `get_frag_list_for_file` + new DIP ioctl |
| `<folder>` / `create=true` | **cIOS DI hook + in-RAM FST rebuild** | hardest | above + FST editor |

Everything above the double line is pure loader-side C/C++ in the existing injection
window. Everything below needs the read to be intercepted *after* the game starts —
i.e. inside cIOS. This is the fork's real cost center.

## 3. Proposed architecture

New module `source/riivo/` in the loader:

- `riivo/RiivoParser.{cpp,h}` — XML → data model. Direct port of
  `riivultimatum/riivo/parser.py` (itself from Dolphin `DiscIO/RiivolutionParser`).
  Model: `Disc`, `Section`, `Option`, `Choice`, `Patch`, and patch kinds
  `File/Folder/Memory/Savegame`.
- `riivo/RiivoConfig.{cpp,h}` — option/choice/section resolution + `${param}`
  substitution (port of the resolver in `parser.py`). Produces a flat
  `ResolvedPatchSet`.
- `riivo/RiivoMemory.{cpp,h}` — Phase 1 engine. Consumes `Memory` patches, applies
  them to `dolList[]` / absolute addresses in the injection window. Port
  branch/ocarina encoding from `dol/gecko.py`.
- `riivo/RiivoFst.{cpp,h}` — Phase 3/4. Parses the game's on-disc FST, resolves
  `File`/`Folder` patches to FST entries, builds a **redirect table**
  `{disc_offset, len, external_file_fraglist}` using `get_frag_list_for_file`, and
  (Phase 4) rebuilds the in-RAM FST to add `create=true` entries.
- `riivo/RiivoMenu.{cpp,h}` — GUI page (GameCube-settings-style, see
  `source/settings/menus/`) to pick an XML under `sd:/riivolution/`, toggle
  options/choices, and stash the selection in `GameCFG`.
- **cIOS side (separate fork of d2x):** new DI ioctl, e.g. `DI_SETREDIRECT`, that
  registers redirect tuples; DIP read handler checks redirects before the normal
  fragment translation and, on a hit, reads from the external file's fraglist.

Data flow at boot:

```
RiivoMenu selection (GameCFG)
  -> RiivoParser + RiivoConfig  => ResolvedPatchSet
  -> [Phase1] RiivoMemory.apply(dolList)      in BootGame injection window
  -> [Phase3] RiivoFst.buildRedirects()       before Disc_JumpToEntrypoint
  -> WDVD_SetRedirect(table)  (new ioctl)      -> d2x DIP
  -> Disc_JumpToEntrypoint()
```

## 4. Phased plan

**Phase 0 — Parser + model + menu (no patching).**
Port parser/resolver; add the GUI to select an XML and resolve options. Deliverable:
log the fully resolved patch set for a game. Pure loader C++, no boot risk. Unblocks
everything.

**Phase 1 — `<memory>` (the quick win).**
Implement all four memory variants in `RiivoMemory`, invoked from `BootGame()` right
after `gamepatches()` (`GameBooter.cpp:733`). Order vs. `load_handler`: apply direct/
search/valuefile before it; for `ocarina` reuse `find_safe_space` and place the
branch so it does not collide with the Gecko handler region (`0x800022A8–0x80003000`).
Deliverable: memory-only Riivolution mods work (RAM hacks, many code patches).
Ships without any cIOS changes.

**Phase 2 — `<savegame>`.**
Redirect the save directory using the existing save-path / NAND-emu redirection
(`source/usbloader/SavePath.cpp`, `NandEmu.cpp`). Self-contained; no cIOS work.

**Phase 3 — `<file>` replacement of existing files.**
Introduce the d2x `DI_SETREDIRECT` ioctl and DIP read hook. Loader parses the on-disc
FST, matches `<file>` patches to entries, builds redirect tuples with
`get_frag_list_for_file`, registers them before jump. Start with single-file replace
where the entry already exists. **This is the phase that forks d2x cIOS** — budget
accordingly. Validate with a known file-swap mod.

**Phase 4 — `<folder>` and `create=true`.**
Enumerate SD folders, match/emit per-file redirects, and rebuild the in-RAM FST to
add new entries (offsets/lengths the game will request), then redirect those too.
Full Riivolution generality. Highest risk (FST layout + string table editing).

## 5. Key risks / open questions

- **cIOS dependency (Phases 3–4).** Options: (a) fork d2x and add `DI_SETREDIRECT`
  (recommended — aligns with existing SETFRAG plumbing); (b) in-RAM IOS DI hook from
  the loader (no cIOS fork, but fragile across IOS versions). Decide before Phase 3.
- **Read latency.** Per-read redirect lookup + SD seek on the game's hot path;
  Riivolution mods tolerate it, but large-file streaming may need a sorted/interval
  table and readahead.
- **FST rebuild safety (Phase 4).** Games cache FST; must rebuild before apploader
  hands off, and keep the string table consistent.
- **`${param}` / disc-id matching** edge cases already handled in `parser.py` — port
  the tests too.
- **GC games** use a different path (`BootGCMode`/Nintendont); Riivolution is Wii-only
  so scope to `TYPE_GAME_WII_*`.

## 6. Reference map

- Dolphin `Source/Core/DiscIO/RiivolutionParser.{h,cpp}` — parser/model spec.
- Dolphin `Source/Core/DiscIO/RiivolutionPatcher.cpp` — memory/FST semantics,
  `ApplyGeneralMemoryPatches` ordering.
- `riivultimatum/riivo/parser.py`, `dol/gecko.py` — your ports to translate to C++.
- d2x cIOS DIP module — read-interception plumbing to extend.
- Loader anchors: `GameBooter.cpp:275/680/733/771`, `gamepatches.c:75/102/218/234`,
  `patchcode.c:46/78`, `apploader.c:73`, `wdvd.c:279/402`, `frag.c:163/270`.

---

# Appendix A — Verified mechanics (deep-dive pass)

Everything below was read in-source or confirmed against the Riivolution spec +
Dolphin `RiivolutionPatcher.cpp`.

## A.1 The exact injection sequence

`BootPartition()` (`GameBooter.cpp:131`) does, in order:
`Disc_FindPartition` → `WDVD_OpenPartition` → **`Disc_SetLowMem`** (`disc.c:44`,
writes the boot low-mem globals; disc ID at 0x80000000) → `Disc_SelectVMode` →
**`Apploader_Run`** (`apploader.c:30`). The apploader loop (`apploader.c:68`) calls
`appldr_main` to get `(dst,len,offset)` tuples, `WDVD_Read`s each into `dst`, and
`RegisterDOL(dst,len)` records it. **The FST is placed in RAM by the apploader; its
pointer lives at `*(u32*)0x80000038`** (used already at `apploader.c:99`). So after
`BootPartition` returns, all DOL sections + the FST are in RAM at final addresses.

Then `BootGame()` (`GameBooter.cpp`): `gamepatches()` (733) → `load_handler()` (736)
→ fix480p/error-codes/Wiimmfi → `Disc_JumpToEntrypoint()` (771).

`Disc_JumpToEntrypoint` (`disc.c:388`): sets vmode/time, **shuts down IOS subsystems
and closes exceptions**, then jumps. If `hooktype != 0`, it enters via the code
handler at **`0x800018A8`** (`bctr`); else it `blr`s straight to `AppEntrypoint`.
⇒ **All Riivolution work must finish before this call.** Memory patches slot in a new
`riivo_apply_memory_patches()` right after `gamepatches()`; the redirect table
(`WDVD_SetRedirect`) must be registered before this too, while DI is still up.

## A.2 Memory-patch semantics (from Dolphin, exact)

- **Address**: effective addr = `patch.offset | 0x80000000`. Riivo offsets are stored
  relative to MEM1 base; OR-ing is idempotent for full `0x80xxxxxx` values.
- **Direct** (`offset`+`value`/`valuefile`, optional `original`): if `original` set,
  byte-compare first and skip on mismatch; else write bytes. Maps to a guarded
  `memcpy` + `DCFlushRange`/`ICInvalidateRange` (same as `gamepatches.c:196`).
- **Search** (`search=true`, needs `original`+`value` same length, `align`): stride =
  `align`; scan the loaded DOL sections for `original`, write `value` at the match,
  stop. This is the `anti_002_fix` idiom (`gamepatches.c:218`) with a variable stride.
- **Ocarina** (`ocarina=true`, `offset`+`value`): scan RAM in 4-byte strides for the
  `value` byte pattern; from the match, scan forward for `blr = 0x4E800020`; at that
  blr write branch `jmp = ((target - blr_addr) & 0x03FFFFFC) | 0x48000000` where
  `target = offset | 0x80000000`. **Note the mask `0x03FFFFFC`** (24-bit displacement,
  low 2 bits are AA/LK and must stay 0) — the loader's `patch_width` used `0x3FFFFFF`;
  use `0x03FFFFFC` in the port. Payload placement can reuse `find_safe_space`
  (`gamepatches.c:234`) when the target is loader-provided scratch.
- **Scan scope on console**: before boot only the DOL is resident (not the full game),
  so search/ocarina scan `dolList[]` sections, exactly like the existing patchers.
  Direct writes go to the absolute `offset|0x80000000`.
- Dolphin splits "general" (direct/search over full RAM) vs "apploader" (ocarina +
  early search) phases only because of *when* it emulates them. On hardware we apply
  once, over the loaded DOL, in the injection window — a single phase.

## A.3 Note: the loader already has a runtime poke engine

`gameconfig.txt` `poke`/`pokeifequal` (`patchcode.c:148 app_pokevalues`,
`:199 app_loadgameconfig`, `:526 LoadGameConfig`) is a *runtime* poke applied by the
code handler after boot — different timing from Riivo `<memory>` (pre-entry). Don't
reuse it for `<memory>`, but it's a proven parser/format precedent for a text config.

## A.4 The frag/DIP protocol (for Phases 3–4)

- `FragList`/`Fragment` (`frag.h:15`): `Fragment{offset,sector,count}` in sector units;
  `FragList{size,num,maxnum,frag[MAX_FRAG=20000]}`.
- `get_frag_list(id)` (`frag.c:265`) builds the game's list; `set_frag_list(id,sdOnly)`
  (`frag.c:270`) ships it via `WDVD_SetFragList(device, list, size)` →
  `IOS_Ioctl(IOCTL_DI_SETFRAG=0xF9, …)` (`wdvd.c:402`). `device` = `WBFS_DEVICE_USB`
  or `2` for SD-only. Called from `SetupDisc()` (`GameBooter.cpp:238-247`).
- **`get_frag_list_for_file(fname,id,part_fs,lba_offset,sector_size)` (`frag.c:163`)**
  already maps an arbitrary FAT/NTFS/WBFS file to a physical-sector `FragList`
  (`_FAT_get_fragments`, `_NTFS_get_fragments`, WBFS). This is the redirect primitive.
- **DI ioctl space**: cIOS commands sit at `0xF4–0xFF`
  (`SETWBFSMODE 0xF4`, `SETFRAG 0xF9`, `GETMODE 0xFA`). Free slots (`0xF7/0xF8/0xFB…`)
  are available for a new **`IOCTL_DI_SETREDIRECT`** that registers
  `{disc_offset, len, external_file_FragList}` tuples. The d2x DIP read handler
  (services `IOCTL_DI_READ 0x71`) checks the redirect table before normal frag
  translation and, on a hit, reads the external file's fragments instead.
- **Device constraint**: cIOS reads the redirect files from the same device layer it
  already uses; redirect targets should live on the game's device (or require d2x's
  SD read path). Confirm during the Phase 3 d2x fork.

## A.5 Savegame redirection (Phase 2)

Saves already route through NAND emulation: `SetupNandEmu()` (`GameBooter.cpp:180`) →
`CreateSavePath()` (`SavePath.cpp:74`) builds `{NandEmuPath}/title/000100xx/{gameid}/
{data,content}` and a `title.tmd`. `<savegame external= clone=>` maps onto pointing
that specific title's data dir at the Riivo external folder and, on first run with
`clone=true`, copying the existing save in. Self-contained; depends on NAND emu being
enabled for the title. No cIOS work.

## A.6 Persistence + GUI integration points

- **GameCFG** (`settings/CGameSettings.h:10`) is a flat struct serialized as
  `key:value;` text (`CGameSettings.cpp:169 Save`, parser branches ~`:370+`). Add two
  string fields — `RiivoPath` (selected XML) and `RiivoConfig` (serialized
  option→choice selection, e.g. `opt1=2,opt2=1`) — with matching `fprintf` + `strcmp`
  parse branches. Trivial, mirrors `NandEmuPath`/`CustomAddress`.
- **Menu**: `GameSettingsMenu::CreateSettingsMenu()` (`GameSettingsMenu.cpp:85`)
  dispatches per-game sub-menus by index (Game Load → `new GameLoadSM`, Ocarina →
  `CheatMenu`, …). Add a "Riivolution" main button (`SetMainButton`, ~`:81`) + a new
  index branch launching `new RiivoMenu(DiscHeader)`. Model the page on `GameLoadSM`
  (subclass `SettingsMenu`, drive an `OptionList`, override `SetOptionNames/Values` +
  `GetMenuInternal`); the Ocarina `CheatMenu` is the model for a *dynamic* list, which
  RiivoMenu needs since options come from the parsed XML at runtime.

## A.7 Corrections to the initial plan

- Ordering: apply direct/search/ocarina memory patches **after `gamepatches()`**; they
  are independent of the Gecko code handler, so `load_handler` order doesn't matter for
  them — but keep any ocarina payloads clear of `0x80001000–0x80003000` (multidol +
  code handler + code list) if hooktype is enabled.
- Use branch mask `0x03FFFFFC`, not `0x3FFFFFF`.
- FST pointer is `*(u32*)0x80000038`, already used by the alternate-DOL path.
