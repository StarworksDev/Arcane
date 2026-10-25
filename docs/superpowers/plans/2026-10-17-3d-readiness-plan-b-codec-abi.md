# 3D-readiness Plan B — Codec and ABI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Execution gates (re-vet 2026-10-19).** Tasks 1-2 run now. Task 3 waits for fix-round Tasks 2-4 and 9-10 on main. Run each plan's Servitor sequence once on an unmodified base build first.

**Goal:** Gate plugin load on a human epoch plus a compiler-derived digest, then move the `.arcart` byte codec into ArcaneAssets so one version constant and one hash serve the cooker and the runtime.

**Architecture:** Task 1 adds the manifest and `kGamePluginABIDigest` without bumping the epoch, and proves a comment-only edit does not move `build/abi-digest.txt`. Task 2 bumps the epoch once, puts `ABIDigest` on `PluginVTable`, and refuses a mismatch before `Init` on both the loaded-DLL path and the static path. Task 3 moves the codec. The digest tool is not the constants generator (that is Plan C). It runs after headers compile, because it asks the compiler.

**Tech Stack:** C++23, premake5, MSBuild, Catch2 v3, PowerShell, a small C++ manifest TU (not a Lua generator).

**Spec:** `D:/dev/starworks/arcane-readiness/2026-10-17-3d-readiness-arc-design.md`, sections 3.3, 3.4, 4 steps 3 and 6, 7, and decision 5. Line numbers below were checked at `309aa06e` and re-verified unchanged on `main` `a12f33e6` (2026-10-19). After Plan A, re-check any line this plan cites before editing it. Behaviour disagreements stop the task.

**Prerequisite gate:** Plan A Tasks 1–3 are merged. Tasks 4–8 have not run. `Tests/ArcaneTests/data/artifacts/v1-texture.arcart` and `v1-mesh.arcart` and their `.sha256` siblings exist. `PhysicsSystem2D::operator()` is still the inline body in `PhysicsSystem.hpp`. `PhysicsBodyRef2D::handle` is still `Detail::Physics2D::BodyHandle` (`PhysicsComponents.hpp:101`). Do not require `uint64` here. Task 1's first `build/abi-digest.txt` records that pre-move, pre-`uint64` layout. Plan A Task 4 then runs this plan's `--check` and does not `--write`. Plan A Task 5, which starts only after this plan's Tasks 1 and 2 have merged, changes the field, rebuilds `arcane-abi-digest`, and only then regenerates the digest with `--write`. Do not bump the epoch in a commit that only moves the codec. The one epoch bump is Task 2, because the export set and `PluginVTable` grow.

## Global Constraints

- **Worktree:** `D:\dev\starworks\Arcane-3dr`, branch cut from `main` `a12f33e6`. `Arcane-f5` is the old F5 branch at `309aa06e`; never build or test there. Never push, never force-push, never merge to `main`.
- **One heavy build at a time across worktrees.** Do not start `build.ps1`, `msbuild`, or `cl.exe` while another worktree on this machine is already compiling. Desk build, from the repo root. Windows PowerShell 5.1 `Start-Process` has no `-Priority`. `-PassThru` returns the process; `PriorityClass` is how 5.1 lowers it. `ExitCode` is the build's code.

```powershell
Remove-Item Env:CI -ErrorAction SilentlyContinue
Remove-Item Env:ARCANE_BUILD_MACHINE -ErrorAction SilentlyContinue
$env:_CL_ = '/MP4'
$build = Start-Process -FilePath powershell.exe -ArgumentList @(
    '-NoProfile','-ExecutionPolicy','Bypass','-File',
    'D:\dev\starworks\Arcane-3dr\scripts\ci\build.ps1','Debug','-Jobs','4'
) -WorkingDirectory 'D:\dev\starworks\Arcane-3dr' -PassThru -NoNewWindow
if (-not $build.HasExited) { $build.PriorityClass = 'BelowNormal' }
$build.WaitForExit()
if ($build.ExitCode -ne 0) { throw "desk build failed: exit $($build.ExitCode)" }
```

`build.ps1` always regenerates (`scripts/ci/build.ps1:39-43`). A new `.cpp` is globbed by premake (`premake5.lua:1316`) and is not in the vcxproj until that generate. An exe from before the generate is the old binary.
- **Tests** run from the exe directory. Record the seed banner. New cases use `--rng-seed 1`. After any `Set-Location` into `Binaries\Debug-windows-x86_64-md\ArcaneTests`, every script path is root-qualified. Suite gate before every commit:

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

Never a bare `Arcane::Runtime`; use `Arcane::Test::Process()`.
- **Red steps.** After writing a new test `.cpp`, run the desk build before `ArcaneTests.exe`. **Compile error:** the build's `ExitCode` is non-zero and `logs\build-tests.log` or `logs\build-engine.log` names the missing declaration. Do not run the previous exe. **Assertion:** the build's `ExitCode` is 0, then the new exe fails a named `CHECK` or `REQUIRE`. Do not write "FAIL, file missing" for a test that calls a symbol that does not exist yet.
- **D3D12 debug layer.** `test_main.cpp` on main forces it and fails any case that raises `RenderErrorTotal` unless the case is tagged `[expects-render-errors]`. Leave it. This plan adds no GPU behaviour.
- **No golden re-bless.** Pixels do not depend on the epoch. Do not run `golden-gate.ps1` as a per-task gate. Run it once at the end of Task 3. Expected: 14 lanes, every `diffCount` 0. Delete `imgui.ini` in both host exe dirs first.
- **Servitor** after Task 2 and after Task 3. Task 1's digest is not visible in a running host until Task 2's load gate refuses a mismatch; Task 1 has no Servitor step for that reason. Binary: `Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe` from this tree. Own `--headless` runtime. Do not pass `--client` to `launch` or `status`. Main refuses it with exit 2. Fix-round Task 21 makes them accept it. The commands below work either way. Never drive an editor the user has open. Never `--bless`. Quit what you launched, including when a later command fails. Record every exit code. Client name `plan-b`. Exit codes: 0 ok, 4 no host, 5 retry, 6 failed, 7 host died, 8 check failed.
- **Style:** `/MD`, no `/fp:fast`, ASCII comments, UTF-8 without BOM. Do not edit `ThirdParty/Astra`. The digest reads `ASTRA_ENTITY_BITS`, `ASTRA_ENTITY_VERSION_BITS`, and `ASTRA_MAX_COMPONENTS` as the compiler saw them (`Entity.hpp:149-153` defaults 32 and 8; `Component.hpp:17-18` default `ASTRA_MAX_COMPONENTS` 128). Do not change those defaults.
- **OpenProject stays a warning.** `Runtime.cpp:822-840` logs and continues. The warning text is lines 837–840. Do not store the digest on the project. Do not make `OpenProject` a refusal. `.arcproj` keeps storing the epoch only.
- **Every commit message ends with:**

```
Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Ertr3dpdimU1VjCXXAJSBi
```

### Line drift (re-verify after Plan A)

Re-verified 2026-10-19 on `main` `a12f33e6`: no cited file changed since `309aa06e`.

| Citation | Code at the 2026-10-17 read |
|---|---|
| `kGamePluginABIVersion` | `Source/Runtime/ArcaneCore/src/Arcane/Core/AbiVersion.hpp:10`, value 61. Task 2 sets it to one above the value at execution time. If Plan A or another merge already moved it, bump that value, not a number copied from this sentence. |
| `PluginVTable` | `PluginABI.hpp:1257-1272`. Eight pointers. `DrawUI` is last and optional. |
| Static table fill | `GameModule.hpp:390-396`. |
| Epoch `static_assert` | `GameModule.hpp:349-355`, macro `ARC_GAME_MODULE_ABI_CHECK_`, only under `ARC_MONOLITHIC`. |
| `GamePlugin_ABIVersion` | `GameModule.hpp:372-373`. |
| Load compare | `Plugin.cpp:59-68`, after required exports (`Plugin.cpp:40-54`) and after optional `DrawUI` (`:57`). |
| Static path comment | `PluginHost.cpp:1075-1082`. "no ABI probe". `InitImage` is called at line 1085. |
| `engineAbi` | `ProjectManifest.hpp` field, loaded from `"engine"."abi"`. Checked-in files that store `"abi": 61`: `ReferenceProject/ReferenceProject.arcproj` and `Tests/ArcaneTests/data/arcbuild-fixture/Fixture.arcproj`. No other `.arcproj` has the field. Restamp both in Task 2 to the new epoch. |
| `kArtifactVersion` | `ArtifactFormat.cpp:14` and `ArtifactReader.cpp:18`, both `constexpr` 1. |
| `SectionTag` | `ArtifactFormat.hpp:76-87`. Tag 7 tangents is reserved. Mesh thumbnail is tag 3, also reserved. |
| Stale "client must not link the pipeline" comments | `ArtifactFormat.hpp:19-27` and `ArtifactReader.hpp:8-18`. The reader already lives in ArcaneAssets (a Runtime module). |
| `HashSourceBytes` | Pipeline: `SourceHash.hpp:38` (inline, FNV offset `14695981039346656037`, prime `1099511628211`). Reader: `ArtifactReader.cpp:688` (anonymous namespace). Task 3 leaves one function. |
| `HotReloadPluginBad` | `Tests/Fixtures/HotReloadPlugin/HotReloadPlugin.cpp:187`, ABI + 999. |
| Spelling-guard header list | `ArcaneSpellingGuardTest.cpp:34-55`, 21 headers. The member scan walks that list. |
| Rebase shift (fix Tasks 2-4) | After the rebase over fix Tasks 2-4, these lines move (measured at `751786bd`; Task 9 will move the reader lines again). `ArtifactFormat.cpp`: 14->15, 15->16, 23->24, 685->660. `ArtifactReader.cpp`: 18->20, 27->29, 520->504, 639->623, 688->672, 759->743, 795->779, 858->842. `MeshImporter.cpp`: 520->526, 733->739. Re-grep each line before editing. |

## Review Focus

1. **A comment-only edit of a listed header does not change the mix.** The digest is `sizeof` / `alignof` / `offsetof` and the three Astra defines, not stripped source text. Pinned in Task 1 by compiling a comment-modified copy of a listed header in a second TU and printing that TU's mix. Calling `AbiManifestMix()` twice in one process does not recompile anything.
2. **`ASTRA_ENTITY_BITS=64` with a matching epoch is refused before `Init`.** The host stays at the default 32 (`Entity.hpp:149-150`). The fixture is a real DLL whose `GamePlugin_ABIDigest` returns `AbiManifestMix()` evaluated in that TU. A child process that links ArcaneEngine calls `Plugin::Load` on it. The parent never `LoadLibrary`s it. A second DLL returns a stale digest at the matching epoch. Pinned in Task 2.
3. **`OpenProject` with a stale `engineAbi` still opens and only warns.** The warning is `Runtime.cpp:837-840`. The test asserts the project opened and that the log line contains `opening`. It does not assert a refusal. Pinned in Task 2.
4. **Exactly one `constexpr std::uint32_t kArtifactVersion` declaration, and exactly one `enum class SectionTag`, after comments and strings are stripped.** Other `constexpr` declarations in the same file do not count. A comment that says `Tangents` does not count. Pinned in Task 3. The checked-in v1 bytes must equal a fresh write.
5. **A static `PluginVTable` whose `ABIDigest` disagrees is refused before `Init`.** `PluginHost.cpp:1075-1090` is the path, and it is compiled only under `ARC_MONOLITHIC`. `Plugin::FromStatic` (`Plugin.cpp:88-95`) copies the table and does not check ABI. The behaviour test calls `AdoptStaticGameModule`, which both the monolithic block and the test use, and asserts the capture-free `Init` counter stayed 0. A source scan is not that test. Pinned in Task 2.

## Out of scope

- Plan C (constants generator, bindless, residency, uploads, the fix #11 completion check).
- Implementing fix #11.
- LODs, clusters, tangent section tag 7 (stays reserved and unwritten), L08 security gaps, raising bindless capacity, Box3D, F5 Plans 2 and 3.
- Storing the digest in `.arcproj` or in the Hub schema.
- Folding the CRT flavor check (`Plugin.cpp:104-128`) into the digest. It stays a separate gate.
- Rewriting `PluginABI.hpp`'s history comments.

## File map

| File | Task | Responsibility |
|---|---|---|
| `Source/Runtime/ArcaneCore/src/Arcane/Core/AbiManifest.hpp` (new) | 1 | `constexpr` `sizeof` / `alignof` / `offsetof` mixes. Both the engine and a game compile it, so `ASTRA_ENTITY_BITS=64` changes the value. |
| `Source/Programs/arcane-abi-digest/src/AbiDigest.cpp` (new) | 1 | The compiler-driven tool. Includes `AbiManifest.hpp`. `--write` writes `build/abi-digest.txt`. `--check` exits non-zero when the file or `kGamePluginABIDigest` disagrees with `AbiManifestMix()`. Not the constants generator (Plan C). |
| `build/abi-digest.txt` (new) | 1 | One line: the lowercase hex of the digest, plus a newline. |
| `Tests/ArcaneTests/src/AbiDigestTest.cpp` (new) | 1, 2 | Comment stability, define/size/offset failure, load refusals, OpenProject warning. |
| `AbiVersion.hpp` | 2 | Epoch becomes previous + 1. |
| `PluginABI.hpp`, `Plugin.cpp`, `GameModule.hpp`, `PluginHost.cpp` | 2 | Vtable slot, compare, static check. |
| `ReferenceProject/ReferenceProject.arcproj`, `Tests/ArcaneTests/data/arcbuild-fixture/Fixture.arcproj` | 2 | `"abi"` restamped. |
| `Source/Runtime/ArcaneAssets/src/Arcane/Assets/ArtifactCodec.hpp` + `.cpp` (new) | 3 | The one codec. |
| `Source/Developer/ArcaneAssetPipeline/ArcaneAssetPipeline.arcmodule` | 3 | Gains a public dependency on `ArcaneAssets`. |

---

### Task 1: Compiler-derived digest, no epoch bump

**Files:**
- Create: `Source/Runtime/ArcaneCore/src/Arcane/Core/AbiManifest.hpp`
- Create: `build/abi-digest.txt`
- Create: `Tests/ArcaneTests/src/AbiDigestTest.cpp`
- Modify: `Source/Runtime/ArcaneCore/src/Arcane/Core/AbiVersion.hpp` only to declare `kGamePluginABIDigest`. Do not change `kGamePluginABIVersion`.

**Interfaces:**
- Consumes: the 21 headers in `kGameFacingHeaders` (`ArcaneSpellingGuardTest.cpp:34-55`). This plan's Tasks 1 and 2 run after Plan A Tasks 1–3 and before Plan A Task 4, so the array still names `Source/Runtime/ArcanePhysics2D/src/Arcane/Scene/PhysicsComponents.hpp` and `PhysicsSystem.hpp` (`ArcaneSpellingGuardTest.cpp:49-50`). `PhysicsSystem2D.hpp` does not exist yet. Read the list from the file. Do not retarget those two paths. Plan A Task 4 replaces the `PhysicsSystem.hpp` entry, and Task 5 moves the component header and the system header to ArcaneScene.
- Produces:

```cpp
namespace Arcane
{
    // Layout mix of the game-facing by-value closure. A game TU that includes
    // this header under different ASTRA_ENTITY_* defines gets a different value.
    // Unadorned. Do not put ARC_CORE_API on this function.
    constexpr std::uint64_t AbiManifestMix() noexcept
    {
        // The body is in this header. See the paragraph under the snippet.
    }

    inline constexpr std::uint64_t kGamePluginABIDigest = /* the mix at the default defines */;
}
```

`AbiManifestMix` is `constexpr` and header-defined so a fixture compiled with `ASTRA_ENTITY_BITS=64` can evaluate it without linking the host's copy. It is not `ARC_CORE_API`. In a modular game TU that macro is `__declspec(dllimport)` (`Api.hpp:7-9`, `ARC_DLL_IMPORT` at `ModuleApi.hpp:4`): `ARC_CORE_EXPORTS` is set only while compiling ArcaneCore, and a game TU defines neither `ARC_MONOLITHIC` nor `ARC_CORE_STATIC`. Defining that imported function in the game TU is ill-formed. The header-defined evaluator stays unadorned, so the game TU compiles its own body. `kGamePluginABIDigest` is already an unadorned `inline constexpr` and stays that way. It is the value at the default defines, and it must equal the integer in `build/abi-digest.txt`. A `static_assert` in the header is **not** added in this task (the monolithic check is Task 2). This task's test compares `AbiManifestMix()` to the file.

The mix is one FNV-1a (offset `14695981039346656037`, prime `1099511628211`) over a stable byte stream:

1. The three defines, each as the ASCII name, a `=`, the decimal value, a newline, in this order: `ASTRA_ENTITY_BITS`, `ASTRA_ENTITY_VERSION_BITS`, `ASTRA_MAX_COMPONENTS`. Read the value with `#ifdef` / the default the header documents. Do not hash a `#define` line of source.
2. For each root type, in the order listed below: the ASCII type name, newline, then `sizeof` as 4 little-endian bytes, `alignof` as 4 little-endian bytes, then each field in declaration order: field name, newline, `offsetof` as 4 little-endian bytes, the field type's `sizeof` as 4, the field type's `alignof` as 4, the ASCII name of the field's declared type, newline. Every type and field name is a string literal written in `AbiManifest.hpp`. Do not derive one from `Astra::TypeID<T>::Name()`. Since Astra `fa5ff853` that is the canonical spelling, and `TypeNameCanonical.hpp` lists data-model-dependent aliases such as `std::int64_t` as still differing across compilers. A derived name would make Linux `build.sh --check` disagree with a Windows `--write`.

Root types (by-value, not behind an opaque pointer). Transitive field types are recorded when the field's declared type is itself a struct in this set, by including that struct as its own record. `Astra::Entity` is in the set because component structs store it.

Start the root list with these, then extend it in this task until the member scan in Step 1 is quiet:

- `Astra::Entity`
- `Arcane::EngineContext` (`PluginABI.hpp:1218`). Record every field. A pointer or function-pointer field is recorded as the ASCII of its actual declared type (for example `Arcane::TypeContext*`, `void*`, `bool (*)(EngineContext*)`), plus `sizeof` and `alignof` of that field type. Do not traverse the pointee. Do not record the token `ptr`. A change from `TypeContext*` to another pointer type moves the digest even when the width stays 8. By-value structs in the set are still their own records.
- Every `struct` in the 21 headers that is not inside a function and whose definition is not a pointer typedef.

The member scan is what makes the list complete. A public struct field whose name does not appear in `AbiManifest.hpp`'s `offsetof` expressions fails the test. Pointer fields and references are recorded as pointer-sized. Arrays are recorded as the array type. Do not skip a field because it looks unused.

`AbiManifest.hpp` includes the headers `kGameFacingHeaders` names when this task runs. It does not add an include of `Physics2DDetail.hpp` or a `Manifold2D/` header of its own. `PhysicsComponents.hpp` still includes `Physics2DHandles.hpp` (`PhysicsComponents.hpp:9`) until Plan A Task 5, and `Physics2DHandles.hpp` is itself one of the 21 (`ArcaneSpellingGuardTest.cpp:55`). That transitive include is expected at this gate. Do not record a Manifold type in the mix. If a public struct field's declared type is a Manifold type, stop and report.

`ProcessContext.hpp` is one of the 21 headers, and fix-round Task 12 changes its private members (`:50-56`). If Task 12 reaches `main` after this task, rebuild `arcane-abi-digest` and run `--check` on the rebased tree. If it fails, `--write` from the rebuilt tool and update `kGamePluginABIDigest` in that rebase commit. Do not bump the epoch.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("abi digest matches build/abi-digest.txt", "[plugin][abi]")
{
    const auto path = Arcane::Test::RepoRoot() / "build/abi-digest.txt";
    REQUIRE(std::filesystem::exists(path));
    std::string file = ReadFileBinary(path);
    if (!file.empty() && file.back() == '\n') file.pop_back();
    std::ostringstream hex;
    hex << std::hex << std::setfill('0') << std::setw(16) << Arcane::AbiManifestMix();
    CHECK(file == hex.str());
}

TEST_CASE("a comment-only change does not move the abi digest", "[plugin][abi]")
{
    // Do not call AbiManifestMix() twice in this process. That does not recompile.
    // A second TU, or a second run of arcane-abi-digest, compiles a copy of one
    // listed header (or of AbiManifest.hpp) that differs only by an appended
    // comment line. The printed uint64 equals the unmodified mix.
    // A source-text hash of the modified copy differs. The mix must not.
}

TEST_CASE("every public struct field is in the abi manifest", "[plugin][abi]")
{
    // Walk kGameFacingHeaders. For each struct field declaration that survives
    // PublicDeclarations stripping, require the field name to appear as
    // offsetof(<Type>, <field>) in AbiManifest.hpp.
}
```

The third test's strip rules match `ArcaneSpellingGuardTest.cpp` `ScanMode::PublicDeclarations`: comments, string literals, preprocessor lines, and `ARC_INTERNAL` fences are ignored. A self-check in the test: the string `"struct Probe { int alpha; };"` yields field `alpha`, and `"// int alpha;"` yields nothing.

Fourth case, in the same file:

```cpp
TEST_CASE("a by-value public parameter is in the abi root list", "[plugin][abi]")
{
    // For each of the 21 headers, after PublicDeclarations stripping, a function
    // declaration whose parameter or return type is a struct (not a pointer,
    // not a reference, not a std::span / std::string_view / std::function)
    // must appear as a root-type name in AbiManifest.hpp.
    // Self-check: "Probe Foo(Probe value);" requires "Probe".
    // "Probe* Foo(const Probe& value);" does not.
}
```

- [ ] **Step 2: Build, then stop on the compile error**

Desk build. `AbiDigestTest.cpp` calls `Arcane::AbiManifestMix`, which does not exist yet. Expected: `ExitCode` is non-zero. `logs\build-tests.log` names `AbiManifestMix` as undeclared. Do not run `ArcaneTests.exe`. That compile error is the red. A missing `build/abi-digest.txt` is not what this run observes, because the TU does not compile.

- [ ] **Step 3: Write the manifest, the tool, and the file**

Implement `AbiManifestMix` as specified. `kGamePluginABIDigest` is the committed default-defines value the host will compare against. Do not change `kGamePluginABIVersion`.

Create the premake console app `arcane-abi-digest` (`Source/Programs/arcane-abi-digest/`, source `AbiDigest.cpp`, which includes `AbiManifest.hpp` and nothing that registers a component). Set `location "Source/Programs/arcane-abi-digest"`, the same pattern as `arcbuild` (`premake5.lua:580`), so the vcxproj is `Source/Programs/arcane-abi-digest/arcane-abi-digest.vcxproj`. It is a target in `Arcane.slnx`. `--write` writes `build/abi-digest.txt` as 16 lowercase hex digits and one newline, equal to `AbiManifestMix()` at the default defines, and the implementer copies that integer into `kGamePluginABIDigest`. `--check` exits non-zero if the file is missing, if its line disagrees with `AbiManifestMix()`, or if `kGamePluginABIDigest` disagrees with the mix. Do not hand-author the hex. `scripts/ci/build.ps1` and `scripts/ci/build.sh` run `--check` after the engine build. A developer who changed a listed header rebuilds this target first, then runs `--write`, updates the constant, and commits both. `--write` from the exe built before that header edit records the old mix. Plan A Task 5 is that sequence.

The comment-stability case compiles the modified copy (a fixture TU that includes the copied header, or a second invocation of the tool pointed at that copy) and requires the printed `uint64` to equal the unmodified mix. Appending the comment in memory and calling `AbiManifestMix()` again is not that case.

Add a test that fails if `kGamePluginABIDigest != AbiManifestMix()`. The `static_assert` of the mix against the constant inside `ARC_GAME_MODULE_ABI_CHECK_` is Task 2, not this task. The tool's `--check` is what fails CI in this task.

The member-scan test must pass. If a field is a bit-field, `offsetof` is ill-formed; record the byte offset of the storage unit and the bit width in the name (`field:3`) and note it in a comment next to that one entry. Do not leave the field out.

- [ ] **Step 4: Build and run**

Desk build. Expected: exit 0.

```powershell
.\ArcaneTests.exe "abi digest matches build/abi-digest.txt" --rng-seed 1
.\ArcaneTests.exe "a comment-only change does not move the abi digest" --rng-seed 1
.\ArcaneTests.exe "every public struct field is in the abi manifest" --rng-seed 1
.\ArcaneTests.exe "a by-value public parameter is in the abi root list" --rng-seed 1
```

Expected: PASS. Then `run-tests.ps1 Debug`. Expected: exit 0. `HotReloadPlugin` still loads (the epoch did not move). No golden gate.

- [ ] **Step 5: Commit**

```
git add Source/Runtime/ArcaneCore/src/Arcane/Core/AbiManifest.hpp Source/Runtime/ArcaneCore/src/Arcane/Core/AbiVersion.hpp Source/Programs/arcane-abi-digest build/abi-digest.txt Tests/ArcaneTests/src/AbiDigestTest.cpp scripts/ci/build.ps1 scripts/ci/build.sh premake5.lua
git commit -m "feat: add the compiler-derived plugin ABI digest"
```

with the trailer.

---

### Task 2: Epoch bump and the load gate

**Files:**
- Modify: `AbiVersion.hpp:10` (epoch + 1)
- Modify: `PluginABI.hpp:1257-1272` (add the function pointer)
- Modify: `Plugin.cpp:40-68` (resolve and compare)
- Modify: `GameModule.hpp:349-396` (export, static table, monolithic assert)
- Modify: `PluginHost.cpp:1075-1085` (check before `InitImage`)
- Modify: `ReferenceProject/ReferenceProject.arcproj` and `Tests/ArcaneTests/data/arcbuild-fixture/Fixture.arcproj` (`"abi"`)
- Modify: `Tests/ArcaneTests/src/AbiDigestTest.cpp`
- Create: `Tests/Fixtures/AbiDigestBits64/` (a real DLL plus a child exe that calls `Plugin::Load`)
- Create: `Tests/Fixtures/AbiDigestStale/` (a real DLL whose digest export returns a stale constant, plus the same child shape)
- Test: `HotReloadPluginBad` still fails at the epoch compare and does not export a digest

**Interfaces:**
- Consumes: `kGamePluginABIDigest` and `AbiManifestMix()` from Task 1. `PluginResolveError::Kind::AbiMismatch` (already the epoch's kind).
- Produces:

```cpp
// PluginVTable, after DrawUI:
std::uint64_t (*ABIDigest)() = nullptr;

// GameModule.hpp, in the game TU. Returns the mix evaluated in THAT TU,
// not the committed host constant. A TU compiled with -DASTRA_ENTITY_BITS=64
// therefore exports a different integer.
ARC_GAME_MODULE_EXPORT std::uint64_t GamePlugin_ABIDigest()
{ return ::Arcane::AbiManifestMix(); }

// kGamePluginABIDigest stays the default-32 value the HOST compares against.

// Shared by Plugin::Load and the static adoption path:
bool AcceptPluginAbi(const PluginVTable& table, PluginResolveError* error) noexcept;

// Compiled in every build, not only ARC_MONOLITHIC. Calls AcceptPluginAbi.
// On false, returns false and does not call Init. On true, the monolithic
// block may call InitImage.
bool AdoptStaticGameModule(const PluginVTable& table, PluginResolveError* error, Plugin* out);
```

`AcceptPluginAbi` returns false with `Kind::AbiMismatch` when `table.ABIVersion` is null, when the epoch disagrees, when `table.ABIDigest` is null, or when `table.ABIDigest()` disagrees with `kGamePluginABIDigest`. It does not call `Init`. `AdoptStaticGameModule` is the function `PluginHost.cpp`'s monolithic block calls instead of going straight from `ArcGameModule_Get()` to `InitImage` (`PluginHost.cpp:1075-1090`). `Plugin::FromStatic` (`Plugin.cpp:88-95`) still only copies the table; the check lives in `AdoptStaticGameModule`, before `InitImage` (`PluginHost.cpp:439-455` is the call that runs `Init`).

Resolve order in `Plugin::Load`:

1. Required exports, unchanged (`Plugin.cpp:40-54`). An old module that lacks `GamePlugin_ABIDigest` but whose epoch already mismatches fails at the epoch compare **before** the new export is required. Implement that by calling `ABIVersion()` and comparing the epoch before resolving `GamePlugin_ABIDigest`.
2. If the epoch mismatches: `AbiMismatch`, return false. Do not require the digest export. `HotReloadPluginBad` (epoch + 999) still hits this branch.
3. If the epoch matches: resolve `GamePlugin_ABIDigest`. Missing symbol is the existing unresolved-symbol failure (`need(...)`), not `AbiMismatch`.
4. If the digest value mismatches: `AbiMismatch`, return false, before `Init`.

`DrawUI` stays optional and is resolved where it is today (after the required seven, before the epoch compare is fine; do not call `Init`).

`ARC_GAME_MODULE_ABI_CHECK_` keeps the epoch `static_assert` and adds:

```cpp
static_assert(::Arcane::AbiManifestMix() == ::Arcane::kGamePluginABIDigest,
              "ARC_GAME_MODULE: digest does not match build/abi-digest.txt");
```

That assert is inside the existing `#if defined(ARC_MONOLITHIC)` macro. A monolithic game built with `-DASTRA_ENTITY_BITS=64` fails at compile time because `AbiManifestMix()` changes and `kGamePluginABIDigest` is the committed default-32 value. The game export still returns `AbiManifestMix()`, not that constant. The host compares the export against the constant.

`PluginHost.cpp:1075-1090`: delete the sentence that there is no ABI probe. The monolithic block calls `AdoptStaticGameModule(*ArcGameModule_Get(), ...)`. On false, log and return false. Do not call `InitImage`. The block is still under `#if defined(ARC_MONOLITHIC)`. `AdoptStaticGameModule` itself is compiled in every configuration so the test can call it.

`PluginEntry` gains `kABIDigest = "GamePlugin_ABIDigest"`.

Restamp both `.arcproj` files' `"engine"."abi"` from 61 to the new epoch. If the constant was no longer 61, restamp from the old value to the new one. Search the tree for `"abi":` in `*.arcproj` and update every hit. Tests that embed `kGamePluginABIVersion` in JSON (`ClientRuntimeTest.cpp:154`, `CVarModuleLifetimeTest.cpp:406`, `BootStageParityTest.cpp`) follow the constant. Do not hardcode 62 in those tests if they already print the constant.

History comment: add one line at the ABI history in `PluginABI.hpp` (the block that records v61, around line 1190): v(N) adds `GamePlugin_ABIDigest` and `PluginVTable::ABIDigest`. Do not rewrite older history entries.

- [ ] **Step 1: Write the failing refusal tests**

```cpp
// Capture-free. A capturing lambda does not convert to uint64_t (*)()
// (PluginABI.hpp:1271) or to bool (*)(EngineContext*).
std::uint32_t TestAbiEpoch() { return Arcane::kGamePluginABIVersion; }
std::uint64_t TestAbiWrongDigest() { return Arcane::kGamePluginABIDigest ^ 0x1ull; }
int g_initCalls = 0;
bool TestAbiInit(Arcane::EngineContext*) { ++g_initCalls; return true; }

TEST_CASE("a matching epoch with the wrong digest is refused before Init", "[plugin][abi]")
{
    g_initCalls = 0;
    Arcane::PluginVTable table{};
    table.ABIVersion = &TestAbiEpoch;
    table.ABIDigest = &TestAbiWrongDigest;
    table.Init = &TestAbiInit;
    Arcane::PluginResolveError err{};
    CHECK_FALSE(Arcane::AcceptPluginAbi(table, &err));
    CHECK(err.kind == Arcane::PluginResolveError::Kind::AbiMismatch);
    CHECK(g_initCalls == 0);
}

TEST_CASE("a static table with the wrong digest is refused before Init", "[plugin][abi]")
{
    g_initCalls = 0;
    Arcane::PluginVTable table{};
    table.ABIVersion = &TestAbiEpoch;
    table.ABIDigest = &TestAbiWrongDigest;
    table.Init = &TestAbiInit;
    Arcane::PluginResolveError err{};
    Arcane::Plugin adopted;
    CHECK_FALSE(Arcane::AdoptStaticGameModule(table, &err, &adopted));
    CHECK(err.kind == Arcane::PluginResolveError::Kind::AbiMismatch);
    CHECK(g_initCalls == 0);
}

TEST_CASE("OpenProject warns and still opens when engineAbi is stale", "[plugin][abi]")
{
    // Write a temp project whose manifest "engine"."abi" is kGamePluginABIVersion - 1
    // and whose game module is absent (content-only). OpenProject returns true.
    // The captured warning contains "opening".
}
```

Use the existing log-capture helper the engine tests already use for `ARC_WARN`. If the only capture is a test sink installed through `Arcane::Log`, use that. Do not assert the process aborted.

Two real DLLs, both built in the same Debug CRT as the child (Plugin.cpp:104-128 refuses a CRT mismatch before any export check). Neither is loaded into `ArcaneTests.exe`. `AbiManifest.hpp` includes the 21 game-facing headers, and those headers' `ARC_REFLECT` registrars plus a different `Entity` size would steal the test process's `TypeContext`. The child is disposable.

1. `Tests/Fixtures/AbiDigestBits64/`. DLL compiled with `-DASTRA_ENTITY_BITS=64`. `GamePlugin_ABIVersion` returns the host epoch (`kGamePluginABIVersion` from `AbiVersion.hpp`, so the epoch matches). `GamePlugin_ABIDigest` returns `AbiManifestMix()` evaluated in that TU, the same body the production macro uses. It does not return `kGamePluginABIDigest`. `GamePlugin_Init` writes a sentinel file if it is called. The other six exports exist and are empty enough to resolve.
2. `Tests/Fixtures/AbiDigestStale/`. DLL at the default entity bits. Epoch matches. `GamePlugin_ABIDigest` returns `1ull`, which is not the current mix. `Init` writes a different sentinel. Do not use `ARC_GAME_MODULE` for this DLL: that macro returns `AbiManifestMix()`. Hand-write the exports.

A child exe links ArcaneEngine, calls `Plugin::Load` on the DLL path it was given, prints the `PluginResolveError` kind, and exits 0 when the kind is `AbiMismatch`. The parent spawns it twice.

```cpp
TEST_CASE("a 64-bit digest DLL and a stale digest DLL are refused before Init", "[plugin][abi]")
{
    const Bits64Run bits64 = RunDigestChild("AbiDigestBits64");
    CHECK(bits64.exitCode == 0);
    CHECK(bits64.kind == "AbiMismatch");
    CHECK_FALSE(std::filesystem::exists(bits64.sentinel));
    const StaleRun stale = RunDigestChild("AbiDigestStale");
    CHECK(stale.exitCode == 0);
    CHECK(stale.kind == "AbiMismatch");
    CHECK_FALSE(std::filesystem::exists(stale.sentinel));
}
```

`RunDigestChild` uses `_popen` on Win32 and `popen` on POSIX. A missing fixture exe is a failure of this task, not a `SKIP`. Both fixture projects are in `Arcane.slnx`. If the child's static initializers abort before it prints a kind, stop and report. Do not replace the DLL load with a synthetic vtable. Header registrars may run inside the child at `LoadLibrary`, before the digest compare; the assertion is still that `Init` did not run and the printed kind is `AbiMismatch`.

- [ ] **Step 2: Build, then stop on the compile error**

Desk build. The new cases call `AcceptPluginAbi` and `AdoptStaticGameModule`, which do not exist yet. Expected: `ExitCode` is non-zero. `logs\build-tests.log` names one of those two as undeclared. Do not run `ArcaneTests.exe`. That compile error is the red. The fixture DLLs are generated by the same `build.ps1`; their absence is not the red of this step, because the test TU does not compile.

- [ ] **Step 3: Implement the gate and the bump**

Read `AbiVersion.hpp:10` and add 1. Update the two `.arcproj` files. `GamePlugin_ABIDigest` returns `AbiManifestMix()`. Fill the static table (`GameModule.hpp:390-396`) with `&GamePlugin_ABIDigest` after `&GamePlugin_DrawUI`. Update the "eight" comments in `GameModule.hpp:361-364` to say nine, including the digest. `ARC_GAME_MODULE` comment that says "eight GamePlugin_*" becomes nine.

`HotReloadPluginBad` (`HotReloadPlugin.cpp:187`, ABI + 999) still fails at the epoch compare before the digest export is required. It does not gain a digest export.

The stale-digest DLL is the loaded-DLL proof of a same-epoch wrong value. The in-process `AcceptPluginAbi` case stays as well, with capture-free function pointers. Delete any capturing lambda. A capturing lambda does not convert to `std::uint64_t (*)()`.

- [ ] **Step 4: Build, tests, Servitor**

Desk build, including the bits-64 fixture, the stale-digest fixture, and ReferenceProject (the `.arcproj` abi must match or the existing project tests fail). Expected: `ExitCode` 0. Then, from the exe directory:

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "a matching epoch with the wrong digest is refused before Init" --rng-seed 1
.\ArcaneTests.exe "a static table with the wrong digest is refused before Init" --rng-seed 1
.\ArcaneTests.exe "OpenProject warns and still opens when engineAbi is stale" --rng-seed 1
.\ArcaneTests.exe "a 64-bit digest DLL and a stale digest DLL are refused before Init" --rng-seed 1
.\ArcaneTests.exe "abi digest matches build/abi-digest.txt" --rng-seed 1
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

Expected: PASS, and `run-tests.ps1` exits 0. The existing HotReload bad-plugin case still fails the load (run the test by its current name; do not weaken it). Record each exit code.

Servitor:

```powershell
$S = 'D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
& $S --project ReferenceProject launch runtime --headless -- --fixed-time 0
& $S --project ReferenceProject --client plan-b --host runtime wait settled
& $S --project ReferenceProject --client plan-b --host runtime quit
```

Expected: each exit 0. `wait settled` exit 0 means the module built from this tree loaded. If it exits 6 or 8, read the envelope. An ABI mismatch on ReferenceProject means the `.arcproj` or the game DLL was not rebuilt. Fix that. Do not make `OpenProject` refuse.

No golden gate in this task.

- [ ] **Step 5: Commit**

```
git add Source/Runtime/ArcaneCore/src/Arcane/Core/AbiVersion.hpp Source/Runtime/ArcaneEngine/src/Arcane/Plugin ReferenceProject/ReferenceProject.arcproj Tests/ArcaneTests/data/arcbuild-fixture/Fixture.arcproj Tests/ArcaneTests/src/AbiDigestTest.cpp Tests/Fixtures/AbiDigestBits64 Tests/Fixtures/AbiDigestStale premake5.lua
git commit -m "feat: refuse a plugin whose ABI digest disagrees"
```

with the trailer. Stage every other `.arcproj` you restamped.

---

### Task 3: One `.arcart` codec in ArcaneAssets

**Prerequisite of this task:** Fix-round Tasks 2, 3, 4, 9 and 10 are on `main`. `ReadFilePrefix` already checks `is_regular_file`, and both IndexData loops already refuse `index >= vertexCount`. The codec keeps both refusals.

**Files:**
- Create: `Source/Runtime/ArcaneAssets/src/Arcane/Assets/ArtifactCodec.hpp`
- Create: `Source/Runtime/ArcaneAssets/src/Arcane/Assets/ArtifactCodec.cpp`
- Modify: `Source/Developer/ArcaneAssetPipeline/ArcaneAssetPipeline.arcmodule` (public dependency `ArcaneAssets`)
- Modify: `ArtifactFormat.cpp`, `ArtifactFormat.hpp`, `ArtifactReader.cpp`, `ArtifactReader.hpp`, `SourceHash.hpp` so the writer and the reader call the codec
- Test: `Tests/ArcaneTests/src/ArtifactCodecGateTest.cpp` (new), plus the existing round trips and `ArtifactV1FixtureTest.cpp`

**Interfaces:**
- Consumes: Plan A's `v1-texture.arcart` and `v1-mesh.arcart`. `kArtifactVersion` is 1 in both old sites. `SectionTag` values 1–7 (`ArtifactFormat.hpp:76-87`). `ContentKind` Texture=1, Mesh=2 (`:93-97`). FNV-1a as in `SourceHash.hpp:38`.
- Produces: one header owns `kArtifactVersion`, `SectionTag`, `ContentKind`, and the prefix layout. Delete both production `HashSourceBytes` definitions: `Arcane::AssetPipeline::HashSourceBytes` (`SourceHash.hpp:38`, inline) and the anonymous one at `ArtifactReader.cpp:688`. One function, `Arcane::HashSourceBytes`, is declared in `ArtifactCodec.hpp` and defined in `ArtifactCodec.cpp`. Update the callers: `TextureImporter.cpp:346`, `MeshImporter.cpp:733`, `ArtifactReader.cpp:795`, `ArtifactReader.cpp:858`. Delete `SourceHash.hpp`'s function, and delete the header if nothing else remains in it. Point `Tests/ArcaneTests/src/AssetsTest.cpp` and `MeshArtifactReaderTest.cpp` at the shared function so the copies cannot drift. Those test files call `Arcane::HashSourceBytes` and do not define it. Tag 7 is not written. The gate under `Source/` counts two different shapes, because a definition is also a declaration: exactly one prototype (a function declarator with no body) and it lives in `ArtifactCodec.hpp`, and exactly one definition (a function declarator with a body) and it lives in `ArtifactCodec.cpp`. Requiring a single declarator of either kind fails on this layout, which has both. `Tests/` is outside that count.

`ArcaneAssetPipeline` is Developer and may depend on Runtime `ArcaneAssets`. `ArcaneAssets` does not depend on the pipeline. Importers, BC7, glTF, `maxSize`, and the supersede rule stay in the pipeline. The shared piece is the prefix, the section table, the version constant, the FNV-1a, and the concatenation order.

Delete the comments that say the runtime reader is an independent reimplementation so the client does not link the pipeline (`ArtifactFormat.hpp:19-27`, `ArtifactReader.hpp:8-18`). Replace them with one sentence: the byte codec lives in ArcaneAssets; the pipeline keeps cook policy.

`build/abi-digest.txt` updates in this commit only if a by-value manifest type changed size, alignment, offset, or field type. A file move does not. If the member scan from Task 1 fails because a type moved headers, update `AbiManifest.hpp`'s include and regenerate the txt only when the mix changed. A comment-only include-path edit that keeps the same `offsetof` list does not change the txt.

- [ ] **Step 1: Write the failing gates**

```cpp
TEST_CASE("one kArtifactVersion and one SectionTag", "[artifact][codec]")
{
    // Strip comments and strings first, the same rules as
    // ArcaneSpellingGuardTest ScanMode::PublicDeclarations.
    // Count declarations that match `constexpr std::uint32_t kArtifactVersion`
    // exactly. Require exactly one, and that it lives in ArtifactCodec.hpp.
    // Other constexpr in ArtifactFormat.cpp (kMagic line 15, kSectionEntrySize
    // line 23) and ArtifactReader.cpp (kSectionEntrySize line 27) do not count.
    // Today the two version declarations are ArtifactFormat.cpp:14 and
    // ArtifactReader.cpp:18, so the count is 2 and this CHECK fails.
    // Count `enum class SectionTag` exactly once, in ArtifactCodec.hpp.
    // After the strip, a writer TU (ArtifactCodec.cpp, ArtifactFormat.cpp,
    // MeshImporter.cpp) that contains the token SectionTag::Tangents fails.
    // Comments may still say the tag is reserved. ArtifactReader.cpp:520 and
    // :639, ArtifactFormat.cpp:685, MeshImporter.cpp:520, and
    // ArtifactReader.hpp:83 mention Tangents in comments and must not fail.
}

TEST_CASE("checked-in v1 bytes match a fresh codec write", "[artifact][codec]")
{
    // Same compare as ArtifactV1FixtureTest: disk bytes == fresh write,
    // and Sha256Hex(disk) == the sibling with one trailing newline stripped.
}

TEST_CASE("source hash is one function and matches the texture fixture", "[artifact][codec]")
{
    int prototypes = 0;
    int definitions = 0;
    // Walk Source/ after the same comment strip. A function whose declarator
    // is HashSourceBytes and whose body is absent is a prototype. A function
    // whose declarator is HashSourceBytes and which has a body is a definition.
    // A definition is not a prototype. Require prototypes == 1, in
    // ArtifactCodec.hpp, and definitions == 1, in ArtifactCodec.cpp.
    // Counting every declarator and requiring 1 cannot pass: the header
    // prototype and the cpp definition are two declarators.
    const auto png = EncodePng(17, 9, GradientPixels(17, 9));
    const auto hashed = Arcane::HashSourceBytes(png);
    const auto art = ReadV1TextureSourceHash(Arcane::Test::RepoRoot() /
        "Tests/ArcaneTests/data/artifacts/v1-texture.arcart");
    CHECK(prototypes == 1);
    CHECK(definitions == 1);
    CHECK(hashed == art);
}
```

`ReadV1TextureSourceHash` uses the reader and returns the `sourceHash` field. `EncodePng` and `GradientPixels` are the copies Plan A Task 3 put in `ArtifactV1FixtureTest.cpp` as file-local functions. Declare them in `Tests/ArcaneTests/src/Helpers/ArtifactV1Fixture.hpp` in this task and include that header from both tests. Do not add a third silent copy. A hash mismatch means the fixture was not hashed with this FNV. Stop. Do not update the fixture bytes to match a new hash.

The red case for this task is the version gate, which only reads source text and compiles today. The hash case calls `Arcane::HashSourceBytes` before that function exists in `ArtifactCodec.hpp`, so it is a compile error if it is in the same TU as the red run. Put the version gate in `ArtifactCodecGateTest.cpp` first. Add the hash case in Step 3, after the function exists, or keep it out of the red TU until then.

- [ ] **Step 2: Build, then run the version gate**

Desk build. The version-gate TU does not call a missing symbol. Expected: `ExitCode` 0. Then, from the exe directory:

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "one kArtifactVersion and one SectionTag" --rng-seed 1
```

Expected: assertion failure. The count of `constexpr std::uint32_t kArtifactVersion` is 2 (`ArtifactFormat.cpp:14` and `ArtifactReader.cpp:18`). That is not a compile error. Do not describe it as "any constexpr in the file."

- [ ] **Step 3: Move the codec**

Put prefix read/write, section table, version check, and `HashSourceBytes` in `ArtifactCodec.cpp`. Delete both old definitions. `ArtifactFormat.cpp` and `ArtifactReader.cpp` call the codec. Leave importer policy where it is. Add `ArcaneAssets` to the pipeline's public dependencies and desk-build (the `.arcmodule` edit needs the regenerate `build.ps1` already does).

Keep the existing refusal tests green: truncated body, bad section range, bad `slotIndex`. They stay in their current files. Tag 7 is still not written. The version gate from Step 1 is the Tangents check: after comment stripping, no writer TU contains `SectionTag::Tangents`. Do not grep the raw file for the word `Tangents`.

- [ ] **Step 4: Gates, suite, Servitor, golden**

Desk build. Expected: `ExitCode` 0. From the exe directory:

```powershell
Set-Location D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "one kArtifactVersion and one SectionTag" --rng-seed 1
.\ArcaneTests.exe "checked-in v1 bytes match a fresh codec write" --rng-seed 1
.\ArcaneTests.exe "source hash is one function and matches the texture fixture" --rng-seed 1
.\ArcaneTests.exe "v1 texture and mesh artifacts match the checked-in bytes" --rng-seed 1
.\ArcaneTests.exe "abi digest matches build/abi-digest.txt" --rng-seed 1
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\run-tests.ps1 Debug
```

Expected: every case PASS, and `run-tests.ps1` exits 0. The hash case's prototype count is 1 and its definition count is 1.

Servitor. `ReadClientArtifact` (`ArtifactReader.cpp:759`) is the runtime reader this task moved. Own headless runtime. `launch` takes no `--client`. Record every exit code. Quit even if a later command fails.

```powershell
$S = 'D:\dev\starworks\Arcane-3dr\Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
& $S --project ReferenceProject launch runtime --headless -- --fixed-time 0
& $S --project ReferenceProject --client plan-b --host runtime wait settled
& $S --project ReferenceProject --client plan-b --host runtime logs --level error
& $S --project ReferenceProject --client plan-b --host runtime quit
```

Expected: every exit 0. `wait settled` exit 0 means the host booted and loaded the project, which reads cooked artifacts through the runtime reader. `logs` must not contain an artifact-read failure for the reference scene. Exit 6 or 7 is a failure. Do not bless.

Golden gate once, Debug, after deleting both `imgui.ini` files. The script path stays root-qualified:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\dev\starworks\Arcane-3dr\scripts\golden-gate.ps1
```

Expected: 14/14, `diffCount` 0. No `--bless`.

- [ ] **Step 5: Commit**

```
git add Source/Runtime/ArcaneAssets Source/Developer/ArcaneAssetPipeline Tests/ArcaneTests/src/ArtifactCodecGateTest.cpp Tests/ArcaneTests/src/AssetsTest.cpp Tests/ArcaneTests/src/MeshArtifactReaderTest.cpp Tests/ArcaneTests/src/ArtifactV1FixtureTest.cpp Tests/ArcaneTests/src/Helpers/ArtifactV1Fixture.hpp build/abi-digest.txt
git commit -m "refactor: share one arcart codec in ArcaneAssets"
```

with the trailer. Include `build/abi-digest.txt` only if the mix changed.

---

## Self-review

- Spec 3.4 Done: digest file compare and a recompiled comment copy (Task 1), HotReloadPluginBad and wrong digest before Init (Task 2), two real DLLs loaded by a child via `Plugin::Load` (Task 2), `AdoptStaticGameModule` leaves `Init` uncalled (Task 2), OpenProject warning (Task 2), a module from this tree loads (Servitor, Task 2), artifact load on the runtime reader (Servitor, Task 3), no golden move (Task 3's single gate).
- Spec 3.3 Done: fixtures already exist from Plan A; fresh write matches (Task 3); one header owns the constants, counted after comment stripping (Task 3); round-trip refusals stay; tag 7 unwritten; one `HashSourceBytes` prototype in `ArtifactCodec.hpp` and one definition in `ArtifactCodec.cpp` (Task 3).
- The by-value public-function scan is Task 1's fourth case. A pointer or reference parameter does not force a root entry. Pointer fields still record their declared type. The field scan is the third case.
- Plan A Task 4 runs `--check` and does not `--write`. Plan A Task 5 rebuilds `arcane-abi-digest` and then regenerates `build/abi-digest.txt` after the handle becomes `uint64`. Plan C owns fix #11's completion check. This plan does not call the arc complete.

## Review fixes

Codex review `plan-review-b-codec-abi.md` (NEEDS REVISION). Each finding is fixed in this plan. None rejected.

1. **`GamePlugin_ABIDigest` returns the host constant (Critical).** The game TU returns `AbiManifestMix()` evaluated in that TU. `kGamePluginABIDigest` stays the default-32 value the host compares against. The monolithic `static_assert` is still `AbiManifestMix() == kGamePluginABIDigest`. Two real DLLs, loaded by a child process through `Plugin::Load`: one built with `-DASTRA_ENTITY_BITS=64` and a matching epoch, one at default bits whose export returns `1ull`. The parent asserts `AbiMismatch` and that the Init sentinel file was not written. The DLL is not loaded into `ArcaneTests.exe`.
2. **Pointer fields recorded as `ptr` (Important).** Each field's actual declared type is recorded as ASCII, including pointer and function-pointer types, plus `sizeof` and `alignof` of that field type. Pointees are not walked. `EngineContext` (`PluginABI.hpp:1218`) is the example.
3. **No compiler-driven writer (Important).** `Source/Programs/arcane-abi-digest/` is a premake console app in `Arcane.slnx`. `AbiDigest.cpp` includes `AbiManifest.hpp`. `--write` writes `build/abi-digest.txt`. `--check` fails when the file or `kGamePluginABIDigest` disagrees with the mix. `build.ps1` and `build.sh` run `--check` after the engine build. The implementer runs `--write` once. The hex is not hand-authored.
4. **Static test never runs the static path (Important).** `AdoptStaticGameModule` is compiled in every build, calls `AcceptPluginAbi`, and does not call `Init` on failure. The monolithic block (`PluginHost.cpp:1075-1090`) calls it before `InitImage`. The test uses a capture-free `Init` that increments a file-scope counter and checks the counter stayed 0.
5. **Capturing lambda cannot fill the vtable slot (Important).** The in-process cases use file-scope functions. The 64-bit and stale cases are real DLL exports, not a lambda closed over a parsed integer.
6. **Comment test does not recompile, and red steps run a stale exe (Important).** The stability case compiles a comment-modified copy and compares that mix. Task 1's red is a compile error (`AbiManifestMix` undeclared) after the desk build. Task 2's red is a compile error (`AcceptPluginAbi` / `AdoptStaticGameModule` undeclared). Task 3's version gate builds cleanly and then fails the assertion that the declaration count is 1, because `ArtifactFormat.cpp:14` and `ArtifactReader.cpp:18` both declare it.
7. **Version gate counts any constexpr and comment hits (Important).** After the spelling-guard comment/string strip, the gate counts `constexpr std::uint32_t kArtifactVersion` and `enum class SectionTag`, each exactly once, in `ArtifactCodec.hpp`. `SectionTag::Tangents` as a use in a writer TU fails. Comments that mention Tangents do not.
8. **Two `HashSourceBytes` functions remain (Important).** Both definitions are deleted. `Arcane::HashSourceBytes` is the one function in the codec, declared in `ArtifactCodec.hpp` and defined in `ArtifactCodec.cpp`. Call sites in the two importers and the two reader checks are updated. The gate counts prototypes and definitions separately and requires one of each. A single declarator count of 1 cannot pass, because the definition is also a declaration. The test-local copies call the shared function and do not define it.
9. **Task 3 has no Servitor check (Important).** After Task 3, this tree's headless runtime is launched with no `--client` on `launch`, `wait settled`, `logs --level error`, and `quit`. Every exit code is recorded. Task 1 has no Servitor step: the digest is not host-visible until Task 2's load gate. Task 2 keeps its module-load Servitor check.

Cross-cutting: the desk build uses `-PassThru`, `PriorityClass = BelowNormal`, and `ExitCode`. Script paths after `Set-Location` into the exe directory are root-qualified.

### Rev 2

Codex re-review `plan-rereview-b-codec-abi.md` (NEEDS REVISION). The partial and both new findings are fixed. None rejected. Items the re-review marked Addressed are unchanged in substance. The game TU still returns `AbiManifestMix()` evaluated in that TU (addressed finding 1). `--write` / `--check` and the CI check stay (addressed finding 3); the added rule is that `--write` runs only after this target is rebuilt.

1. **`ARC_CORE_API` on the header-defined mix (new).** `AbiManifestMix` is an unadorned `constexpr` defined in `AbiManifest.hpp`. `ARC_CORE_API` expands to `__declspec(dllimport)` in a modular game TU (`Api.hpp:7-9`, `ModuleApi.hpp:4`). Defining that imported function there is ill-formed. `kGamePluginABIDigest` was already unadorned and stays that way. The 64-bit fixture still compiles the body in its own TU and does not return the host constant.
2. **Uniqueness gate cannot pass (partial finding 8, and the new impossible gate).** Both old definitions are still deleted and the callers are still updated. The gate no longer requires one declarator. It requires one prototype, in `ArtifactCodec.hpp`, and one definition, in `ArtifactCodec.cpp`. A definition is not counted as a prototype. `Tests/` stays outside the count. The version-gate red is still the `kArtifactVersion` count of 2 (addressed finding 6); the hash case is still added after the function exists.

Sequencing alignment with Plan A's rev 2, not a separate finding: this plan's Tasks 1 and 2 now require Plan A Tasks 1–3, not 1–4. The first digest is taken while `operator()` is still inline and `handle` is still `BodyHandle`. `kGameFacingHeaders` still names `PhysicsSystem.hpp` and the Physics2D `PhysicsComponents.hpp`. The transitive `Physics2DHandles.hpp` include stays expected until Plan A Task 5. A public Manifold field type is still a stop.

### Rev 3 (controller)

- Codex round 3: Task 3 commit step staged only `ArtifactCodecGateTest.cpp` from the test tree. Fixed: Step 5 also stages `AssetsTest.cpp`, `MeshArtifactReaderTest.cpp`, `ArtifactV1FixtureTest.cpp` and `Helpers/ArtifactV1Fixture.hpp`.
