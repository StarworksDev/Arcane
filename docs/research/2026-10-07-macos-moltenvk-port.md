# macOS port on MoltenVK: design, gaps and status

*2026-10-07 (drafted) / 2026-10-08 (completed). Branch `mac/port`, draft PR #5
into `linux/port`. Verified only through GitHub Actions `macos-15` (arm64,
Apple Clang from Xcode 16.4, libc++); the authoring boxes are Windows and Linux.*

## 1. Scope and non-goals

- **In:** every workspace project builds natively on Apple silicon with premake
  `gmake` + Apple Clang (ArcaneCore, ArcaneAssetPipeline, arccook, arcbuild,
  ArcaneClient, ArcaneServer, ArcaneRuntime, ArcaneEditor, ArcaneTests, the
  test plugins and the death fixture), and ReferenceProject builds through
  `arcbuild build` exactly as an external game would.
- **In:** a Darwin implementation of each platform seam that Linux grew in
  PRs #2-#4: paths, executable/library paths, process liveness, module load +
  hot reload, the loaded-image census, and the POSIX crash/hang path.
- **In:** rendering through the existing **Vulkan** backend on **MoltenVK**.
- **Out:** a Metal backend. D3D12 + Vulkan stay the only two NRI backends;
  MoltenVK is the Mac's Vulkan driver, the same way lavapipe is CI's Linux one.
- **Out:** x86-64 macOS. The premake rule builds for the host architecture
  (`ARCANE_MAC_ARCH` overrides) and nothing tests Intel Macs.

## 2. Build

| Topic | Decision |
|---|---|
| Generator | premake5 beta8 `gmake`, `--cc=clang`; the same Make backend arcbuild uses on Linux (arcbuild's macOS default action is `gmake`; `xcode4` stays available). |
| Architecture | native arm64 (`bin/<Config>-macosx-AARCH64-md/`); `build/arcane.lua` applies the same rule to game modules. |
| Shared libraries | `.dylib` with `@rpath` install names. `runpathdirs` gives every binary an rpath to its own directory first (premake emits it as `@loader_path`), so a staged dylib beside the exe wins, then to its linked siblings' dirs; `libArcaneClient.dylib` adds one to SDL3's lib dir. |
| Whole-archive | ld64's `-force_load <lib>` for imgui (GNU's `--whole-archive` elsewhere); no `-z defs` (ld64 has no equivalent flag). |
| Game modules | still named `<Name>.dll` in `Binaries/` by the shared manifest contract; dyld loads any Mach-O regardless of extension. |
| Shaders | SPIR-V only (no DXIL on a Mac); `dxc` and `libdxcompiler.dylib` come from the pinned LunarG Vulkan SDK, which is also where MoltenVK + the loader come from. |
| SDL3 | built from the pinned source release (`scripts/build-sdl3.sh`), as on Linux. |

## 3. Platform layer (behind `Arcane/Platform/*`)

| Seam | Linux | macOS |
|---|---|---|
| Executable path | `/proc/self/exe` | `_NSGetExecutablePath` + `realpath` |
| Library path of a handle | `dlinfo(RTLD_DI_LINKMAP)` | dyld image list + `RTLD_NOLOAD` probe |
| Module image extent | link_map + `dl_iterate_phdr` | dyld image header, slid `LC_SEGMENT_64` union; matched by handle **or file identity** (see 6.2) |
| Process liveness/CPU | `/proc/<pid>/stat` | `proc_pidinfo` (BSD + task info, Mach timebase) |
| User data dir | `$XDG_DATA_HOME` | `~/Library/Application Support` |
| Loaded-image census | `dl_iterate_phdr` | dyld image list; `/System` and `/usr` are the OS tree |
| File watching | polling `last_write_time` (portable) | same |
| Process spawn | `fork`/`execv` (arcbuild), `pipe2` | same; `pipe` + `fcntl(FD_CLOEXEC)` where Darwin has no `pipe2` |
| Crash/hang capture | PR #3's POSIX design | same design; Mach specifics below |

Crash path on Darwin (`Base/Posix/*`): the stored context is the Mach machine
context (a Darwin `ucontext_t` only points at one); other threads are captured
with `thread_suspend` + `thread_get_state` instead of a snapshot signal; memory
is read with `mach_vm_read_overwrite`; thread ids are Mach port names from
`task_threads`; the minidump carries `MD_OS_MAC_OS_X`, ARM64/AMD64 contexts,
`dyld_all_image_infos` modules with `LC_UUID` RSDS records and Mach exception
codes; `P_TRACED` (sysctl) is the debugger check. The fatal set adds
**SIGTRAP** on macOS: Apple silicon traps with `brk`, so `__builtin_trap`,
libc's fortify check and runtime traps die by SIGTRAP, not SIGILL/SIGABRT.

Apple libc++ gaps handled in-tree:
- `std::from_chars` for floating point is missing: `Arcane::FromChars`
  (`Util/CharConv.hpp`) falls back to C-locale `strtod_l`.
- No IANA tzdb (`std::chrono::current_zone`): the crash report's local stamp
  uses `FormatSystemLocalStamp` (C library `localtime_r`) there.
- `std::filesystem::file_time_type`'s rep is `__int128`, which has no ostream
  operator, so tests do not let Catch2 decompose file-time comparisons.

## 4. Rendering on MoltenVK

- **Instance:** `VK_KHR_portability_enumeration` + the
  `ENUMERATE_PORTABILITY` flag whenever the loader offers it (no `#if`: a
  conformant driver is enumerated either way). `VK_EXT_metal_surface` is an
  optional surface extension.
- **Device:** a portability device must enable `VK_KHR_portability_subset`;
  its feature struct is queried and passed back verbatim, and every feature
  it reports `VK_FALSE` is **logged at device creation** ("VK_KHR_portability_subset
  gaps: ..."), so the gap list comes from the device rather than a hand-kept table.
- **Surface:** on Cocoa the window is an SDL Metal window, and its
  `CAMetalLayer` (from `SDL_Metal_CreateView`) is handed to NRI's VK swapchain
  (`VK_USE_PLATFORM_METAL_EXT` on Mac builds of NRI). Off Cocoa (the offscreen
  driver) a macOS window carries no graphics API at all, because SDL 3.2
  defaults a flagless Mac window to OpenGL, which offscreen cannot load.
- **Hard requirements:** the engine's NRI contract still hard-requires
  timelineSemaphore, bufferDeviceAddress, hostQueryReset, synchronization2,
  dynamicRendering and maintenance4, and reports by name any one that MoltenVK
  lacks.

### 4.1 MoltenVK gaps (measured)

Read off the macos-15 [gpu] lane (MoltenVK from Vulkan SDK 1.4.321.0, adapter
"Apple Paravirtual device"); the first two rows come from the device's own
log line, the rest from NRI's `[nri] caps:` line.

| Gap | Value on the runner | Effect on Arcane |
|---|---|---|
| `VK_KHR_portability_subset` features reported VK_FALSE | `pointPolygons`, `samplerMipLodBias`, `tessellationIsolines`, `tessellationPointMode` | none: no pass uses point-mode polygons, LOD bias or tessellation |
| Plain (non-update-after-bind) sampled-image limits | 640 per set, 128 per stage | the mesh pass's 256-entry bindless table must be built update-after-set, which MoltenVK supports (1,000,000 / 1,000,000); `NriDeviceCaps::SupportsBindlessTextures` now gates on the path the table is actually built with (it read the plain limits only and refused MeshNode) |
| NRI bindless tier | 1 | enough: the engine needs tier > 0 |
| Mesh shaders | not offered | none: no mesh-shader pass exists |
| Ray tracing | tier 0 | none (non-goal for the shipped tiers) |

A real Apple-silicon Mac (Metal argument-buffer tier 2) may report higher
plain limits; nothing in the engine depends on that.

## 5. CI

`macos-appleclang` x Debug/Release/Dist in the one `ci.yml` (docs/ci.md).
The suite runs `~[gpu]` with two seeds under SDL's **cocoa** driver (the
runner has an Aqua session), and Debug adds the `[gpu]` lane on MoltenVK over
the runner's paravirtual Apple GPU. Exclusions live in
`scripts/macos-test-exclusions.txt`, one reason per line: the Windows-only
crash reporter/monitor cases shared with Linux, the None-device D3D12
removal-latch case, the hang witness H1, and, on the [gpu] lane only, the five
editor witness goldens E2-E6 (Metal on the paravirtual device rasterises
editor text differently from the Windows desk's reference; the runtime goldens
match).

## 6. Notable fixes found only on the runner

1. **Mosaic ARM64 SIMD** (`Simd/Bits.hpp`): `__crc32cd` needs `<arm_acle.h>`;
   `__builtin_prefetch` needs a constant locality. Same patch as Astra's
   macOS port; upstream Mosaic needs it too
   (`docs/research/macos-upstream-patches/Mosaic-arm64-apple-clang.patch`).
2. **Plugin image range:** every plugin copied under `/var/folders` reported
   "no image range" (dyld spells the path `/private/var/...`), so its system
   factories were refused. `Module::Image` now also matches the dyld image by
   `equivalent()` (device + inode).
3. **ImGui shortcuts in tests:** `ConfigMacOSXBehaviors` swaps Ctrl and Super
   on Apple; tests inject the platform's Ctrl through `TestKeys::AddKeyEvent`.
4. **Astra headers:** Astra mac/port's Apple-silicon cache line (128),
   `MAP_HUGETLB` guard and Darwin thread-cache link fix, carried verbatim until
   the next Astra sync.
