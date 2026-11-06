# Settings inventory (read-only audit, 2026-10-03)

**FROZEN 2026-10-05 (S5-2): names, audiences, scopes and apply modes below are binding for S6. A change after this line is a new review, not an edit.** First pass 2026-10-03 (read-only); reconciled (S5-1); corrected by the user (S5-2, see "User review"). Spec: `docs/superpowers/specs/2026-10-03-settings-and-cvar-completion-design.md`, s10 and s16.3. Nothing was changed in code. Three read-only audits ran in parallel, one per part below.

**How to review:**
- Skim the SETTING rows. Correct any verdict, name, audience (Editor/Game/PlayerSafe/Server, +Dev), scope (Pref-M machine-wide / Pref-P per-project / Project) or apply mode (Live/NextWorld/Restart) you disagree with.
- Mark rows directly in this file, or tell me "row X: ...".
- S5 of the plan finalizes this file with your corrections before any conversion starts.

## Summary

| Part | SETTING | CONSTANT | DERIVED | OTHER-STORE |
|---|---|---|---|---|
| Part 1: ArcaneCore and the vendored library configs | 92 | 101 | 28 | 1 |
| Part 2: ArcaneClient, ArcaneRuntime, ArcaneServer, ArcaneCrashReporter | 148 | 104 | 32 | 0 |
| Part 3: ArcaneEditor and ArcaneHub | 221 | 40 | 100 | 14 |
| **Total** | **461** | **245** | **160** | **15** |

(SETTING rows were 82 / 126 / 207 = 415 at the freeze; the post-freeze amendments below split rows and add the names the sweeps registered.)

These counts are after S5-1 (R1-R4 + s16.11 applied) and the user's S5-2 review: the Reconciliation below is applied to every part-table row, the scope vocabulary is Pref-M / Pref-P / Project, each category has exactly one settings struct (and each struct one category, since `ARC_SETTINGS` registers `<category>.<field>`), every SETTING name is concrete (wildcard families are enumerated member by member), and the Part 3 "Editor Dev" px metrics that ARE UI chrome are DERIVED from `editor.ui.scale`. The px rows that are NOT UI chrome stay SETTING (the user's S5-2 ruling): render-target extents and texel counts, the grid LOD fade, zoom-scaled canvas-space geometry (graph node/pin metrics, the shader chain layout, the asset graph node geometry), and the asset row/ref thumbnails; the lint exempts them by name. `scripts/settings-inventory-lint.ps1` checks all of it (rules L1-L10; exit 0 = reconciled) and prints these counts. Earlier counts: first pass 481 / 233 / 111; after the reconciliation was written but before S5-1 applied it, 447 / 239 / 111 (the lint counts that file as 455 / 240 / 111 / 15). They are rows by verdict, so a grouped row (e.g. "keepalive idle / interval / probes") counts once. Part 3 also has a separate shortcut table, which is not in the counts.

## Should anything NOT be exposed? (spec s16.8)

**Yes.** About a third of the candidates are real constants. Changing them would break a file, a protocol or a shader, or would be a bug. Merged across all three parts, the categories are:

1. **File, report and blob formats:** artifact magic/version, scene JSON version, snapshot magic, VerifyReport schema, ini section names, recents format versions. This also covers authored-data defaults that an absent JSON key resolves to (Camera, Fixture, Sprite pivot, mesh topology).
2. **Wire and process protocols:** message-id ranges, the length-prefix width, watchdog/reporter exit codes, window-message ids, drag-drop and clipboard payload keys.
3. **Shader/HLSL contracts:** bindless capacity, cull thread-group size, cbuffer slots and packing, Vulkan binding offsets, max selected ids, instance flag bits.
4. **ABI:** the plugin ABI version and entry names, cvar flag bits, InputSnapshot arrays, RenderGraph handle bits, D3D12SDKVersion.
5. **Hardware, OS and graphics-API limits:** feature level 12_0, Vulkan 1.3, SM 6.5, 256-byte cbuffer alignment, the OS console-close cap, path length rules, max texture size.
6. **Static crash-path capacity:** the crash arena, frame/buffer limits, the log backlog ring and the module table. The crash path cannot allocate or read cvars.
7. **Math identities and float tolerances:** pi/tau, FNV constants, Rec.709 and sRGB/WCAG transfer constants, epsilons, the camera pitch lock.
8. **Security and crypto:** salt/hash length, entropy sample size, log-preview truncation (widening it leaks tokens), RFC 4122 bits, the runtime console's player permission.
9. **Test oracles and automation:** the ImageCompare cascade, settle intervals, fault-injector values, run-input flags (`--frames`, `--probe`).
10. **ID spaces and sentinels:** node/pin id bases, 0xFFFFFFFF markers, fixed preview GUIDs.
11. **Vendored-library compile-time constants:** Manifold2D `kLinearSlop`, `kMaxRotation`. Changing them forks the library.
12. **Values whose change would be a bug:** DRED always on, memory zero-init off, `Theme::kNone`, the dock split clamp, the permanent Viewport panel.

So a guard that fails on EVERY new number constant would be wrong. The recommended guard form is in s16.8 / s10.3 (a): a new constant is either a setting or carries an `ARC_CONSTANT("why")` marker.

## Reconciliation (2026-10-03, after an independent review)

The three parts were audited in parallel, so some values were inventoried twice under different names with conflicting metadata. Everything below was checked against the code. **These resolutions override any row in the part tables.** S5 applies them mechanically, BEFORE the user's row-by-row review, so no value gets corrected twice.

### R1. One name per value (canonical names)

| Value | Was (conflicting) | Canonical | Notes |
|---|---|---|---|
| Thumbnail size / time / FOV / retries (MaterialPreviewHarvester.cpp) | `assets.thumbnail.*` (Part 2) vs `editor.assets.thumbnail*` (Part 3) | `editor.thumbnail.size`, `.time`, `.meshFovDegrees`, `.maxRetries` (EditorThumbnailSettings, Editor Dev, **Project**, Restart) | Part 2's rows were dropped (out of its scope). Project scope: thumbnails are a shared cache and goldens depend on them. The checker cell and light stay DERIVED (Part 3). |
| Gizmo snap steps and size | `editor.viewport.snap.*` / `editor.viewport.gizmo.*` (Part 2) vs `editor.gizmo.snap.*` / `editor.gizmo.size` (Part 3) | `editor.gizmo.snap.translate`, `.rotateDegrees`, `.scale`, `editor.gizmo.size` (EditorGizmoSettings, Pref-P) | Part 3 is right: `gizmoSize` lives in the layout ini (ViewportSettings), not a cvar. It migrates. |
| Splash | `splash.*` (Part 1) vs `app.splash.*` (Part 2) | `app.splash.enabled`, `.image`, `.backgroundColor`, `.showProgress`, `.minDurationSeconds` (SplashSettings, Game, Project, Restart) | (0.05,0.05,0.06) and 0x0D0D0F are one colour: ONE default. Moves out of `.arcproj` (R3). |
| Viewport grid | GridNode `editor.viewport.grid.majorEvery` SETTING (Part 2) vs ViewportGrid "major every 10" CONSTANT (Part 3); two structs | ONE `EditorGridSettings`: `editor.viewport.grid.majorEvery`, `.lineColor`, `.minorAlpha`, `.majorAlpha` | Same concept, two renderers: the 3D GridNode (depth-tested) and the 2D ViewportGrid (ortho lines). One family, read by both. Mode-only differences get `grid3D.*` / `grid2D.*` sub-keys. |
| Axis colours (4 spellings) | `editor.viewport.axisColor.*`, `editor.viewport.grid.axis{X,Y}Color`, `editor.theme.axisX/Y/Z`, gizmo `kColorX/Y/Z` | `editor.theme.axisX/Y/Z` (theme tokens) | Only the inspector bars become DERIVED from the tokens (S5-2 review, decision 1 option A: the grid and gizmo axis colours stay values marked `ARC_CONSTANT("pending axis unification re-bless")`, and a post-sweep task unifies them with one deliberate re-bless; this supersedes ruling I4 for those two consumers). |
| Selection outline colours | separate `render.outline.*Color` settings | DERIVED from `editor.theme.amber` (select) and `editor.theme.graphNodeHoverBorder` (hover) | Exact matches: (1,0.65,0.10) = kAmber; (0.25,0.70,1) = kGraphNodeHovBorderColor. Outline WIDTHS stay settings. |
| Fixed step vs server tick | `sim.fixedHz` with two metadata sets | `sim.fixedHz` (SimSettings, Game, Project, **NextWorld**, [10,480], det Y) **and** `server.tickHz` (ServerSettings, Server, Project, Restart, [1,240]) | A client and a server can differ. |
| Frame-delta clamp | `sim.maxFrameDeltaSeconds` (Part 2) vs `editor.play.maxSimDtSeconds` (Part 3) | ONE `sim.maxFrameDeltaSeconds` (det Y), read by the runtime AND editor Play | Two values would let Play-in-editor diverge from shipped behaviour. |
| Shader compile debounce (4 copies: RuntimeApp.cpp:311, ShaderCompiler.cpp:405, ShaderCompiler.hpp:111, EditorApp.cpp:689) | `render.shader.compileDebounceSeconds` vs `editor.shader.compileDebounceSeconds` | `render.shader.compileDebounceSeconds` (Game Dev, Pref-P) | Moves the settle frame: identical default required. |
| Idle / minimized sleep | `render.window.minimizedSleepMs` vs `editor.perf.idleSleepMs` | `app.window.minimizedSleepMs` (Game) + `editor.perf.backgroundFps` (new, Editor) | Two different behaviours, kept apart on purpose. |
| Copy-flash duration | `diagnostics.reporter.copyFlashSeconds` vs `editor.ui.copyFlashSeconds` | `ui.copyFlashSeconds` | The reporter receives it through its command line (it has no registry). |
| Crash log tail lines | `diagnostics.reporter.logTailLines` vs `editor.crash.logTailLines` | `diagnostics.logTailLines` | One value for the reporter and the editor's crash document. |
| Preview lights | `editor.preview.light*` on two different values (MeshDocument (0.4,1,0.3); MaterialSpherePreview (0.45,0.7,0.8)) | ONE `editor.preview.light.direction`, `.ambient`, `.colour` | The mesh document's odd light was drift. The default becomes the shared (0.45,0.7,0.8). The mesh document's look changes, so that is called out at its conversion. |
| `log.level` | Preferences (Part 1) vs Pref-P (Part 3); the line cited as Log.cpp:261 / :258 | `log.level` (LogSettings, Game, Pref-P, Live) at Log.cpp:258-263 (the ARC_CVAR spans those lines) | |
| `console.historySize` | Game (Part 1) vs Editor (Part 3) | Game (Pref-P) | It is registered in Core and serves both consoles. |
| Struct names | `JobSettings` vs `JobsSettings`; strays (`editor.outliner.slowClickMaxSeconds` in InspectorSettings; `editor.ui.tableRowHeight` in AssetBrowserSettings) | `JobsSettings`; **one struct per prefix**: `editor.outliner.*` -> OutlinerSettings, `editor.ui.*` -> EditorUiSettings | |
| The two ConsoleModels | "unbounded growth" was ambiguous | Core `Config/ConsoleModel::m_lines` is the unbounded one -> `console.maxLines`. The editor `Panels/ConsoleModel` is already a 512-line ring -> `editor.console.ringLines` | |

### R2. Policy resolutions (proposed; the user may override at review)

- **Diagnostics cannot be turned off in shipped builds.**
  - Evidence capture (DRED, the crash handler, the hang watchdog) stays on in every shipped build.
  - `diagnostics.installCrashHandler` and `diagnostics.hangWatchdog` become **Dev, never archived, command-line only** (`--set` for debugger sessions).
  - The reporter UI is a preference, not evidence: `diagnostics.spawnReporter` stays a Game setting. Dumps are written regardless.
- **`log.pattern` is CONSTANT.** The editor Console categorises lines by parsing the log prefix (`Panels/ConsoleModel.cpp:18`, `:48`). Changing the pattern would break that.
- **Gravity is NextWorld** until Manifold2D gains a setter (Manifold2D-first, then a re-vendor). The apply vocabulary stays three modes (Live, NextWorld, Restart); "Live via world rebuild" is NextWorld.
- **Scope has two fields.** "Scope" says where the DEFAULT is edited (the Preferences or Project window). The override rung follows the audience:
  - `PlayerSafe` overrides always write the per-machine User rung (`GameUserDir` in Dist), even when the default lives in Project. Examples: `render.backend`, `render.vsync`, window size, tearing, anisotropy, input deadzones.
  - The spec (s3.3, s8) is amended to say so.
- **The scope vocabulary is `Pref-M` / `Pref-P` / `Project` everywhere.** In Parts 1-2, "Preferences" reads as Pref-P unless the row is theme/fonts/keys/layout (Pref-M). Every Pref-M row needs the EditorUser rung, so **S2 builds that rung before S4** (theme, keys, fonts, layouts).
- **Input thresholds are det Y.** `input.pressThreshold`, `.holdSeconds`, `.tapSeconds` and the deadzones decide which actions fire from raw device input. Recorded raw input replays differently if they change.
- **Capacity hints are CONSTANT with an `ARC_CONSTANT` marker.** Reserve sizes and I/O chunks have no observable preference. Six rows were reclassified.
- **Template data and the Hub are OTHER-STORE.** Template content belongs in shipped template assets. The Hub (Rust/Tauri) keeps `settings.archub` and `theme.css`, and a JSON bridge comes later. A fourth verdict, `OTHER-STORE`, keeps them out of the cvar count.

### R3. Second stores (proposed)
- **`.arcproj`** keeps identity only (name, GUID, ABI, game module, boot scene; spec s16.7). Its `physics.gravity` and splash blocks move to `Config/physics.json` and `Config/app.json`. On first open, the existing block is migrated into the Project rung and removed from `.arcproj` (with a Hub check: the Hub reads only identity).
- **`protocol.json` `settings`** (ports, caps, timeouts, token length, max message size) become `net.*` Server cvars. During migration, protocol.json's `settings` object is read as a Project-rung layer. The code-side duplicates go: `MAX_PAYLOAD_SIZE` vs `max_message_size`; `HasToken() >= 64` vs `token_length`.
- **Networking lives in Core** (`Net/`), and its consumers are the Aphelyon services, not ArcaneServer. That is why Part 2 found "no net.* in ArcaneServer" while Part 1 has about 15 `net.*` rows. Both are correct.
- **`D3D12SDKVersion = 619`** is defined twice (Runtime main.cpp:24, Editor main.cpp:43). It stays CONSTANT, moved into one shared header (a must-match pair).

### R4. Pulled forward (not settings work)
- **`Message::ToString` password exposure: latent, not live.** It would include the first 100 payload characters, enough for a Login password. But a 2026-10-03 search found **no caller** in Arcane, Aphelyon (including the live services, `Source/Services`) or the Gacha archaeology copy, and the services never log payloads. So no existing log holds credentials, and no purge is needed. The fix stays: redact auth-message payloads in `ToString`, so the trap can't be sprung later. It is a small Core fix, separate from the settings tranches.
- **Debug HUD in Dist:** a per-configuration default (on in Debug/Release, off in Dist), the same pattern as the validation cvars. Goldens are captured from Debug/Release, so they keep it on.

### User review (S5-2, 2026-10-05)

The user's corrections, applied after the Reconciliation. Each line: the row, what changed, the user's words. The user answered the controller's question on 2026-10-05 16:27; the table lists the changes in the order the answers gave them. Row numbers are rows of this file after the review.

| row (file:line) | field | was | now | user |
|---|---|---|---|---|
| ViewportGrid.hpp:62-63 (grid fade px) | verdict | DERIVED (editor.ui.scale x base) | SETTING `editor.viewport.grid.fade{In,Full}Px`, Editor Dev / Pref-M / Live, `>0` | "Settle as recommended" (carry item 1: rows that are NOT UI chrome go back to SETTING) |
| ShaderEditorDocument.cpp:1687 (checker kCell) | verdict | DERIVED | SETTING `editor.shader.previewCheckerCell`, Editor Dev / Pref-M / Live, `4..128` | same |
| GraphCanvasStyle.hpp:57, :62-63 (pin ring) | verdict | DERIVED | SETTING `editor.graph.pinRing.{width,outerGap,outerWidth}` (GraphPinRingSettings), Editor Dev / Pref-M / Live | same (graph-canvas px, zoom-scaled) |
| ShaderEditorDocument.cpp:391-392 (kNodePadX/Y) | verdict | DERIVED | SETTING `editor.graph.nodePadding`, Editor Dev / Pref-M / Restart, `0..24` | same |
| ShaderEditorDocument.cpp:3401-3416 (pass-chain layout) | verdict | DERIVED | SETTING `editor.shader.chainLayout.{originX,originY,pitchX,sceneOffsetX,sceneOffsetY}` (ShaderChainLayoutSettings) | same |
| ShaderEditorDocument.cpp:3534, :6064, :6199 (pass-name field) | verdict | DERIVED | SETTING `editor.graph.passNameFieldWidth`, Editor Dev / Pref-M / Live | same |
| ShaderEditorDocument.cpp:3534, :6064, :6199 (param-name field) | verdict | DERIVED | SETTING `editor.graph.paramNameFieldWidth`, Editor Dev / Pref-M / Live | same |
| ShaderEditorDocument.cpp:3534, :6064, :6199 (swizzle-mask field) | verdict | DERIVED | SETTING `editor.graph.swizzleFieldWidth`, Editor Dev / Pref-M / Live | same |
| ShaderEditorDocument.cpp:3583 (pass thumb) | verdict | DERIVED | SETTING `editor.shader.passThumbPx`, Editor Dev / Pref-M / Live | same |
| ShaderEditorDocument.cpp:5925 ... :6137 (neutral-pin field, 1 lane) | verdict | DERIVED | SETTING `editor.graph.constPinNeutralWidth1`, Editor Dev / Pref-M / Live | same |
| ShaderEditorDocument.cpp:5925 ... :6137 (neutral-pin field, 2 lanes) | verdict | DERIVED | SETTING `editor.graph.constPinNeutralWidth2`, Editor Dev / Pref-M / Live | same |
| ShaderEditorDocument.cpp:5925 ... :6137 (neutral-pin field, 3-4 lanes) | verdict | DERIVED | SETTING `editor.graph.constPinNeutralWidth4`, Editor Dev / Pref-M / Live | same |
| ShaderEditorDocument.cpp:5925 ... :6137 (Float / param default, 1 lane) | verdict | DERIVED | SETTING `editor.graph.constFloatWidth`, Editor Dev / Pref-M / Live | same |
| ShaderEditorDocument.cpp:5925 ... :6137 (Float2 / param default, 2 lanes) | verdict | DERIVED | SETTING `editor.graph.constFloat2Width`, Editor Dev / Pref-M / Live | same |
| ShaderEditorDocument.cpp:5925 ... :6137 (Float4 / Color / param default, 3-4 lanes) | verdict | DERIVED | SETTING `editor.graph.constFloat4Width`, Editor Dev / Pref-M / Live | same |
| ShaderEditorDocument.cpp:5925 ... :6137 (param range field) | verdict | DERIVED | SETTING `editor.graph.constParamRangeWidth`, Editor Dev / Pref-M / Live | same |
| AssetGraphPanel.cpp:358-361, :365, :395-398, :479, :1807 (node geometry, pin radius, node pads, overflow wire, label pad) | verdict | DERIVED | SETTING `editor.assetGraph.node.{minWidth,maxWidth,headerHeight,accentBarWidth,padding}` (AssetGraphNodeSettings), `editor.assetGraph.{pinRadius,overflowWireThickness,labelPad}` (AssetGraphSettings), Editor Dev / Pref-M / Live | same (canvas-space, zoom-scaled). Dash, legend swatch, strip and combo widths stay DERIVED (screen-space chrome) |
| CrashReportDocument.cpp:168 (window first size) | verdict | DERIVED | SETTING `editor.crash.initialSize`, Editor / Pref-M / Live | "Settle as recommended" (carry item 1: split the mixed `editor.crash.*` row so the window size is SETTING) |
| CrashReportDocument.cpp:262, :387 (field width, text rows) | split | one DERIVED row | field width 320 px stays DERIVED; the 16-line text-row COUNT is a new SETTING `editor.crash.textRows`, Editor / Pref-M / Live, `4..200` | same (a count is not px chrome; S6-40 owns it beside `editor.crash.maxRows`) |
| GridNode.hpp:124-126, Gizmo.cpp:58-60, ViewportGrid.hpp:72-73 (grid and gizmo axis colours) | verdict | DERIVED from `editor.theme.axis{X,Y,Z}` | CONSTANT, `ARC_CONSTANT("pending axis unification re-bless")` | "All as drafted (Recommended)" (decision 1, option A). Only the inspector bars (EditorWidgets.cpp:243-247) bind to the axis tokens |
| Reconciliation R1, axis colours | note | grid, gizmo and inspector bars derive from the tokens | only the inspector bars derive | same |
| `editor.gizmo.color.*`, `debug.physics.{draw,color,style}.*`, `editor.theme.inputPill.*`, `editor.theme.graph.{node*,group*,pin*,category.*}`, `editor.theme.assetGraph.legend*`, `editor.theme.assetKind.*`, `editor.graph.lod.*`, `editor.shader.previewChecker*`, `editor.mesh.primitiveRanges.*` | name | wildcard families | every member named (the lint now refuses a wildcard, rule L10; the member lists were read off the sources, ranges `capsuleLengthRatioStep` dropped as a drag speed) | "Settle as recommended" (carry item 3; also `editor.camera.distanceScaledSpeed (+ .refDistance, .floor)`, whose parenthesised shorthand the emitter dropped, is now `editor.camera.{distanceScaledSpeed,refDistance,floor,speedCap}`, the row's unnamed 1000 cap getting the name `speedCap`) |
| RuntimeApp.cpp:311 vs EditorApp.cpp:689 (`render.shader.compileDebounceSeconds`) | range | `0..2` vs `0..5` | `0..2` on both | "Settle as recommended" (carry item 5: take RuntimeApp.cpp's 0..2) |
| AssetReferenceField.cpp:22 and the other kept-SETTING px rows | why text | mixed separators | one form: "kept SETTING: owned by S6-xx" | same |
| DERIVED / CONSTANT / OTHER-STORE rows (156 rows) | placeholder cells | `-` and `—` mixed; det `-`; stray audience, scope, apply, struct cells on DERIVED rows (e.g. Gizmo.cpp:28-39, HostConfig.hpp:387, GpuContext.cpp:25) | `—` everywhere; det `N` (or `Y`); the governing setting is named in the name cell in parentheses | same |
| `scripts/settings-inventory-lint.ps1` | L9 exemption | `^editor\.thumbnail\.` | only `editor.thumbnail.size`; the restored px rows are exempt by expanded name | same |
| `scripts/settings-inventory-lint.ps1` | `-EmitFrozen` | wrote the names file before the violation check | emits only after the check passes | same |
| ServerApp.cpp:102-103, :164, :170 (pacing) | why text | "Follows sim.fixedHz" | already "Follows server.tickHz" (S5-1 fix round 1); no change needed | same |

Drafting decisions 1-5 (the S5-2 message): "All as drafted (Recommended)".
1. (A) S6 binds only the inspector bars to `editor.theme.axisX/Y/Z`. The grid and gizmo axis colours stay values marked `ARC_CONSTANT("pending axis unification re-bless")`, and a post-sweep task unifies them with one deliberate re-bless. This supersedes integration ruling I4 for those two consumers; S6-21 and S6-31 therefore DO hold the grid and gizmo axis colours under `ARC_CONSTANT`.
2. Outline widths are named `render.outline.*`.
3. Nested categories get their own struct (`log.file.*` -> LogFileSettings, ...).
4. The 2D grid's "major every 10" stays CONSTANT.
5. `ui.copyFlashSeconds` goes in a new `ui` category (UiSettings).

The S5-1 carry items 1-5 (the controller's agenda for the review): "Settle as recommended".
1. Rows that are not UI chrome go back from DERIVED to SETTING: previewCheckerCell, the grid fade px rows and the graph-canvas px rows (zoom-scaled). True UI-chrome px rows stay DERIVED from `editor.ui.scale`. The mixed `editor.crash.*` row is split.
2. The mismatched struct names are kept (e.g. `render.vulkan.*` -> RenderDeviceSettings).
3. Every wildcard family is enumerated member by member.
4. The implementer's calls are accepted: Mosaic `g_logLevel` is DERIVED from `log.level`; `diagnostics.logTailLines` is Game Dev / Pref-P / Live; `editor.console.*` -> EditorConsoleSettings.
5. Every listed row defect is fixed (see the table), and the lint's `-EmitFrozen` ordering is fixed.

## Findings that need the user's eyes

**Spec corrections:**
- **Manifold2D has no gravity setter.** Gravity cannot be Live without a Manifold2D change, which is Manifold2D-first, then a re-vendor.
- **`sim.fixedHz` must be NextWorld:** PhysicsSystem captures dt at creation.
- **The shortcut counts are wrong.** There are about 49 ImGui key checks in 11 files, plus 22 raw-scancode bindings and 7 keys inside the vendored node editor.
- **New categories to add** to s5.2: audio, app.splash, boot, console, runtime, sim, debug.physics, assets.thumbnail.

**Decisions:**
- **Shortcut keys: physical or logical?** The viewport/camera keys read SDL scancodes, which are physical positions (on AZERTY, "W" is the Z key). Everything else reads ImGui keys, which are logical. Recommendation:
  - commands bind logical keys, which match their labels;
  - fly-camera movement binds physical keys, which keep the WASD shape.
- **UI scale.** About 60 hard pixel sizes ignore font size and scale. Recommendation: derive them from one `editor.ui.scale` multiplier instead of about 100 individual Dev cvars.
- **Second stores:**
  - the `.arcproj` `physics.gravity` and splash blocks;
  - `protocol.json` `settings` (ports, caps, timeouts);
  - the Hub's `settings.archub`.

  Recommendation: `.arcproj` physics/splash move to `Config/<category>.json`, and identity stays in `.arcproj`. `protocol.json` settings become `net.*` cvars, with protocol.json read as a Project rung during migration. The Hub keeps its own store, with a JSON bridge later.
- **Capacity hints** (reserve sizes, I/O chunk sizes) were marked SETTING Dev by the "when in doubt" rule. Recommendation: make them CONSTANT with a marker.

**Bugs and gaps found, outside settings** (to queue separately):
- **Latent security issue:** `Message::ToString` would include up to 100 payload chars, enough for a Login password (Protocol.hpp:413). It has NO caller today, so no log holds credentials; it gets redacted anyway (Reconciliation R4).
- **Debug HUD in shipped builds:** the runtime debug HUD draws in every configuration, including Dist.
- **The parallel physics solver is never enabled:** `PhysicsWorld::SetExecutor` is never called.
- **`log.level` never reaches Astra/Manifold2D logs:** `Mosaic::SetLogLevel` is never called.
- **Physics debug arrows are 20 m and 28 m long** (pre-metres leftovers).
- **Sprite-texture overflow passes silently:** it flattens sprites, logs one ERROR, and still exits 0.
- **Every game uses engine identity:** the window is titled "Arcane Runtime", the base input context is "demo", and it boots on the engine splash.
- **No player settings exist yet:** fullscreen, display mode, master volume. Spatial audio is forced off.
- **Menus show wrong shortcuts:** Redo shows only Ctrl+Y; Rename/Delete only fire with the Outliner focused.
- **The same default is spelled in several places:** the fixed step in 6 places (one with a digit fewer), the shader debounce in 4, and the preview light in 2 different ways. The sweep deletes the copies.
- **Unbounded growth:** Core's `Config/ConsoleModel::m_lines` grows without bound (the editor's `Panels/ConsoleModel` is already a 512-line ring), and diagnostics reports have no retention.

---

## Post-freeze amendments (S6, recorded at S6-45; for the user at S6-GATE)

The freeze binds names, audiences, scopes and apply modes. These rows changed after it, each by a controller ruling during the S6 sweeps or by S6-45's coverage check, so `scripts/settings-frozen-names.txt` (regenerated by `settings-inventory-lint.ps1 -RequireFrozen -EmitFrozen`, never by hand) matches what the engine registers. Most are the same fix: a frozen row held two or more values under one name, which breaks one name per value (spec s14.1 step 5.1; precedent: the S5-2 constFieldWidths split). Each amended row says "(S6-45 amendment)".

| Frozen row (was) | Now | Why | Authority |
|---|---|---|---|
| `debug.physics.islandPalette` (rgba[8]), `debug.physics.narrowphaseColors` (7 RGBA) | `debug.physics.color.island0`..`island7`, `debug.physics.color.narrowphase0`..`narrowphase6` | no array CVarType (spec s3); one name per value | ruling S6-10 |
| `debug.physics.trace.lineThickness` row carried the emphasis value too | + `debug.physics.trace.emphasis` (1.0, 0..1) | one name per value | ruling S6-10 |
| `editor.layout.factory.assetsInspectorShare` (1144 / 392) | `editor.layout.factory.browserRefPx`, `editor.layout.factory.assetsInspectorRefPx` | two values under one name | ruling S6-32 |
| `diagnostics.reporter.windowSize` (1000 x 640) | `diagnostics.reporter.windowWidth` (320..7680), `diagnostics.reporter.windowHeight` (240..4320) | the `--window WxH` flag; the ranges are S6-4's, recorded here; scope stays Pref-P (confirmed) | ruling S6-4, controller 06:04 |
| `diagnostics.logTailLines` on two rows with ranges 0..10000 and 20..5000 | both 0..10000 | one value, one range; the code declares 0..10000 (0 = no excerpt) | controller 06:04 |
| `console.windowSize` (640 x 280) | `console.windowWidth`, `console.windowHeight` (64..8192) | two values under one name (S6-24 registered the pair) | S6-45 |
| `log.server.pattern` row held the console AND file patterns | + `log.server.filePattern` | one name per value (S6-3 registered both) | controller 05:09 |
| `runtime.hud.show` Game Dev | Game | a Dist player can turn the HUD on | ruling R4, controller 09:39 (S6-25) |
| `editor.input.liveHighlight` (0.12 + 0.2·v) | `editor.input.liveHighlightBase`, `editor.input.liveHighlightGain` | two values under one name | S6-45 |
| `editor.sprite.ppuRange` (0.5, 1..4096) | `editor.sprite.ppuDragSpeed`, `editor.sprite.ppuMin`, `editor.sprite.ppuMax` | three values under one name | S6-45 |
| `editor.theme.channelMarkers` (Color x4, Editor Dev) | `editor.theme.channelR` / `G` / `B` / `W` (Editor) | four values under one name; Editor, not Editor Dev, like every other theme token (they travel in the .arctheme presets) | S6-45 |
| `editor.viewport.fallbackExtent` (1280 x 720) | `editor.viewport.fallbackExtentW`, `editor.viewport.fallbackExtentH` | the row already named the split (S6-32); the lint's L9 exemption follows the names | S6-45 |
| `editor.preview.sphereSegments` row held 24 x 32 | + `editor.preview.sphereRings` | two values under one name (S6-35 registered both) | S6-45 |
| `render.debug.pendingCookChecker` (a colour pair) | + `render.debug.pendingCookCheckerAlt` | two values under one name | controller 05:03 (S6-16) |
| `render.mesh.defaultLight.*` | + `render.mesh.defaultLight.intensity`, `.ambientIntensity` (0..16) | colour x intensity split | controller 07:05 (S6-19) |
| (none) | `editor.mesh.capsuleRatioDragSpeed` (0.02) | the primitive-ranges row dropped the drag speed from its family; S6-35 registered it | S6-45 |
| (none) | `net.defaultPort`, `net.tokenLength`, `net.sessionLifetimeSeconds`, `net.idleTimeoutSeconds`, `net.heartbeatIntervalSeconds` | protocol.json's `settings` keys, layered over net.* (S6-12) | S6-45 |
| (none) | `server.cheats`, `server.cheatsAllowed`, `server.allowClientSetServer` | the spec's engine knobs (s3.2, s9), registered by the registry since S1 | S6-45 |
| (none) | `editor.settings.saveDebounceMs`, `.openAtBoot`, `.openCategory`, `.keysConflictsOnly` | the settings windows' own cvars (S3, S4-14) | S6-45 |
| (none) | `physics.events.hitThreshold` | 2D physics hit threshold; PhysicsSystem reads it Live (spec 2026-10-08 s7.5) | task A3, 2026-10-08 |
| (none) | `log.remote.ringLines` | how many log lines the remote log ring retains (the sequenced log ring, LogRing.cpp; Dev) | Servitor P0 task 2, 2026-10-13 |
| (none) | `remote.server.frameBudgetMs` | milliseconds of remote commands a host drains per tick (the remote dispatcher, Dispatcher.cpp; Dev; spec s4.3) | Servitor P0 task 8, 2026-10-14 |
| (none) | `remote.server.queueDepth` | remote requests a host queues before it answers host.busy queue-full (Dispatcher.cpp; Dev; ruling B-1) | Servitor P0 task 8, 2026-10-14 |
| (none) | `editor.remote.enable` | whether the editor serves the remote-control surface on 127.0.0.1 (EditorAppFrame.cpp; Dev; spec 0.4(b)). Inventoried as an "Existing cvars (e)" row beside `editor.automation.windowedFrameCapture`, not as a frozen name: ArcaneTests does not compile EditorAppFrame.cpp, and the forward sweep-coverage case requires every frozen name to be registered in the test process (Task 11 controller ruling A) | Servitor P0 task 11, 2026-10-14 |

Frozen names S6-45 registered in code (no inventory change): `editor.camera.floor` (S6-30 spelled it `speedFloor`), `editor.preview.light.colour` (was `.color`), `editor.shader.previewCheckerLight` / `previewCheckerSpriteScale` (were `editor.preview.checkerLight` / `checkerExtent`), `editor.graph.nodePreviewMinPx` / `dragSpeed` / `rangeDragSpeed` (were under `editor.shader.*`), `editor.mesh.primitiveRanges.*` (were `editor.mesh.subdivMax` ...; the minima are now settings too, each floored at ValidateMeshAsset's rule), and the rows no sweep had converted: `astra.snapshot.compression`, `editor.gizmo.color.*`, `editor.graph.const*Width` / `paramNameFieldWidth` / `passNameFieldWidth` / `swizzleFieldWidth`, `editor.shader.chainLayout.*`, `editor.shader.passThumbPx`, `editor.crash.initialSize` (a Vec2) / `textRows`, `editor.ui.toolbar.logoScale` / `brandScale`, `editor.viewport.grid.fadeInPx` / `fadeFullPx`.

## Part 1: ArcaneCore and the vendored library configs

## Settings inventory: ArcaneCore + vendored library configs (read-only audit, 2026-10-03)

Path prefixes: `Core/` = `ArcaneCore/src/Arcane/`, `TP/` = `ThirdParty/`. In CONSTANT and DERIVED rows, the cvar, struct, audience, scope, apply, range and det columns are `-`.

### Config

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Config/CVarRegistry.cpp:123-125 | `console.historySize` | 64 (lines) | SETTING | console.historySize | ConsoleSettings | Game | Pref-P | Live | [1,1024] | N | already a cvar; needs audience/scope metadata (R1: Game, Pref-P; registered in Core, it serves both consoles) |
| Core/Config/ConsoleModel.cpp:10 | `cap` fallback | 64 (lines) | DERIVED | — | — | — | — | — | — | N | shadow copy of the console.historySize default; delete in the sweep |
| Core/Config/ConsoleModel.hpp:45 | `m_lines` | unbounded (lines) | SETTING | console.maxLines | ConsoleSettings | Game | Pref-P | Live | [0=unbounded,1e6] | N | new row (R1): the Core console model grows without bound; 0 keeps today's behaviour (reconciled R1) |
| Core/Config/CVarTypes.hpp:36-46 | CVarFlags bits | 1<<3..1<<13 | CONSTANT | — | — | — | — | — | — | N | flag ABI |
| Core/Config/CVarTypes.hpp:67-73 | SetBy rung ordinals | 10..70 | CONSTANT | — | — | — | — | — | — | N | layer-precedence contract |
| Core/Base/Log.cpp:258-263 | `log.level` default | Init(level), info by default (Log.hpp:25) | SETTING | log.level | LogSettings | Game | Pref-P | Live | [0,6] | N | already a cvar (R1: the ARC_CVAR spans Log.cpp:258-263) |
| Core/Base/Log.cpp:262-263 | `log.level` min/max | 0..6 | CONSTANT | — | — | — | — | — | — | N | spdlog level enum |
| Core/Base/LogRing.cpp | remote log ring capacity | 4096 (lines) | SETTING | log.remote.ringLines | (ARC_CVAR) | Editor Dev | Pref-M | Live | [1,max] | N | how many log lines the remote log ring retains; read when the ring is first touched and on each publish |

### Base / Diagnostics / Log

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Base/Diagnostics.hpp:68 | `Config::appName` | "Arcane" | DERIVED | — | — | — | — | — | — | N | host identity, set per host |
| Core/Base/Diagnostics.hpp:73 | `Config::dumpDir` | "" -> `<exe>/diagnostics` | SETTING | diagnostics.dumpDir | DiagnosticsSettings | Game Dev | Pref-P | Restart | path | N | where reports land |
| Core/Base/Diagnostics.cpp:383 | default subfolder | "diagnostics" | CONSTANT | — | — | — | — | — | — | N | path convention that docs/tests/Hub look in |
| Core/Base/Diagnostics.hpp:78 | `hangSeconds` | 12 (s) | SETTING | diagnostics.hangSeconds | DiagnosticsSettings | Game | Pref-P | Restart | [1,600] | N | slow machines and cold shader compiles |
| Core/Base/Diagnostics.hpp:107 | `gpuStallSeconds` | 8 (s) | SETTING | diagnostics.gpuStallSeconds | DiagnosticsSettings | Game | Pref-P | Restart | [1, hangSeconds) | N | must stay below hangSeconds (comment at :93-99) |
| Core/Base/Diagnostics.hpp:113 | `installCrashHandler` | true | SETTING | diagnostics.installCrashHandler | DiagnosticsSettings | Game Dev | Pref-P | Restart | bool | N | debugger and third-party handler conflicts; Dev, never archived, command-line only (R2) |
| Core/Base/Diagnostics.hpp:114 | `startHangWatchdog` | true | SETTING | diagnostics.hangWatchdog | DiagnosticsSettings | Game Dev | Pref-P | Restart | bool | N | toggle; Dev, never archived, command-line only (R2) |
| Core/Base/Diagnostics.hpp:119 | `productName` | "" -> appName | DERIVED | — | — | — | — | — | — | N | falls back to appName |
| Core/Base/Diagnostics.hpp:124 | `reporterPath` | "" -> `<exe>/ArcaneCrashReporter.exe` | SETTING | diagnostics.reporterPath | DiagnosticsSettings | Game Dev | Pref-P | Restart | path | N | path choice |
| Core/Base/Diagnostics.cpp:2317 | reporter exe name | "ArcaneCrashReporter.exe" | CONSTANT | — | — | — | — | — | — | N | shipped binary name |
| Core/Base/Diagnostics.hpp:128 | `unattended` | false | DERIVED | — | — | — | — | — | — | N | from --headless |
| Core/Base/Diagnostics.hpp:133 | `spawnReporter` | true | SETTING | diagnostics.spawnReporter | DiagnosticsSettings | Game | Pref-P | Restart | bool | N | user may not want a reporter window |
| Core/Base/Diagnostics.hpp:151 | `launchMonitor` | false | DERIVED | — | — | — | — | — | — | N | host sets it to !headless |
| Core/Base/Diagnostics.hpp:156 | `commandLine` | "" | DERIVED | — | — | — | — | — | — | N | host-sanitised argv |
| Core/Base/Diagnostics.hpp:160 | `logDir` | "" -> `<report dir>/../Logs` | SETTING | log.dir | LogSettings | Game Dev | Pref-P | Restart | path | N | path choice |
| Core/Base/Diagnostics.cpp:2286 | default log subdir | "Logs" | CONSTANT | — | — | — | — | — | — | N | path convention |
| Core/Base/Diagnostics.cpp:2288 | log file name | appName + ".log" | DERIVED | — | — | — | — | — | — | N | from appName |
| Core/Base/Diagnostics.hpp:164 | `exitSeconds` | 30 (s) | SETTING | diagnostics.exitSeconds | DiagnosticsSettings | Game Dev | Pref-P | Restart | [0=off,600] | N | `<=0` disables (Diagnostics.cpp:1995) |
| Core/Base/Diagnostics.hpp:168 | `crashHandlingTimeoutSeconds` | 60 (s) | SETTING | diagnostics.crashHandlingTimeoutSeconds | DiagnosticsSettings | Game Dev | Pref-P | Restart | [5,600] | N | timeout |
| Core/Base/Diagnostics.cpp:3031-3032 | 0 -> `60u*1000u` | 60000 (ms) | DERIVED | — | — | — | — | — | — | N | shadow copy of the default above |
| Core/Base/Diagnostics.cpp:95,104 | `kGpuBeatFreshnessCapSeconds`, `*0.5` | 2.0 (s) | DERIVED | — | — | — | — | — | — | N | min(2, gpuStallSeconds/2) by construction |
| Core/Base/Diagnostics.cpp:948-949 | minidump `MINIDUMP_TYPE` | ThreadInfo, HandleData, UnloadedModules, IndirectlyReferencedMemory | SETTING | diagnostics.minidumpKind | DiagnosticsSettings | Game Dev | Pref-P | Restart | enum{Small,Default,Full} | N | full dumps for deep debugging |
| Core/Base/Diagnostics.cpp:1329 | `FlushFileSinkBounded(2000)` | 2000 (ms) | SETTING | diagnostics.logFlushTimeoutMs | DiagnosticsSettings | Game Dev | Pref-P | Restart | [100,10000] | N | crash-path budget; snapshot at Install |
| Core/Base/Diagnostics.cpp:2020 | watchdog poll | 250 (ms) | SETTING | diagnostics.watchdogPollMs | DiagnosticsSettings | Game Dev | Pref-P | Restart | [10,1000] | N | detection resolution |
| Core/Base/Diagnostics.cpp:2132 | watchdog join wait | 5000 (ms) | SETTING | diagnostics.watchdogJoinTimeoutMs | DiagnosticsSettings | Game Dev | Pref-P | Restart | [100,30000] | N | when in doubt |
| Core/Base/Diagnostics.cpp:3052 | `kMinFatalWaitMs` | 5000 (ms) | SETTING | diagnostics.minFatalWaitMs | DiagnosticsSettings | Game Dev | Pref-P | Restart | [1000,60000] | N | when in doubt |
| Core/Base/Diagnostics.cpp:2610 | env `ARCANE_BUILD_MACHINE` / `CI` / `ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE` | env toggles | OTHER-STORE | (fold into diagnostics.spawnReporter as a CommandLine/env rung) | — | — | — | — | — | N | an environment rung, not a new cvar (reconciled). toggle that bypasses the cvar store |
| Core/Base/Diagnostics.cpp:225-231 | `kPathMax`, `kReasonMax`, `kMaxFrames`, `kSectionRsv`, `kHeaderRsv`, `kEnvRsv`, `kEnvLeanRsv` | 1024, 1024, 96, 32 KiB, 8 KiB, 64 KiB, 8 KiB | CONSTANT | — | — | — | — | — | — | N | static crash-path capacity; the crash path cannot allocate or read cvars |
| Core/Base/Diagnostics.cpp:275-307 | snapshot buffers | 128, 4096, 256, 2048, 64, 8192 (chars) | CONSTANT | — | — | — | — | — | — | N | static crash-path capacity |
| Core/Base/Diagnostics.cpp:284 | `kInjectedMax` | 32 | CONSTANT | — | — | — | — | — | — | N | static crash-path capacity |
| Core/Base/Diagnostics.cpp:1693 | catchable-type scan bound | 16 | CONSTANT | — | — | — | — | — | — | N | MSVC EH ABI parse bound |
| Core/Base/Diagnostics.cpp:1810, :3152 | stack guarantee | 64 KiB | CONSTANT | — | — | — | — | — | — | N | must cover the SEH filter (OS/hardware) |
| Core/Base/Diagnostics.cpp:2103, :2671 | watchdog / crash thread stacks | 128 KiB / 256 KiB | CONSTANT | — | — | — | — | — | — | N | crash-path stack sizing |
| Core/Base/Diagnostics.cpp:2228-2229 | console-close wait | 160 x 25 ms = 4 s | CONSTANT | — | — | — | — | — | — | N | OS cap: Windows kills about 5 s after CTRL_CLOSE |
| Core/Base/Diagnostics.cpp:2855 | session-record retry | 50 (ms) | CONSTANT | — | — | — | — | — | — | N | one-shot retry pause, not a preference |
| Core/Base/Diagnostics.cpp:2501 | `"%s\\%s-pid%lu.session"` | name pattern | CONSTANT | — | — | — | — | — | — | N | monitor contract |
| Core/Base/Diagnostics.cpp:422 | `kReportKinds` | strings | CONSTANT | — | — | — | — | — | — | N | envelope vocabulary |
| Core/Base/Diagnostics.hpp:175-178 | ExitCode | 10..13 | CONSTANT | — | — | — | — | — | — | N | monitor/CI/reporter contract |
| Core/Base/DiagEnvelope.hpp:34 | `kFormatVersion` | 1 | CONSTANT | — | — | — | — | — | — | N | file format |
| Core/Base/CrashArena.hpp:46 | `kCapacity` | 256 KiB | CONSTANT | — | — | — | — | — | — | N | static crash arena |
| Core/Base/CrashArena.cpp:87,95 | printf slot | 512 (B) | CONSTANT | — | — | — | — | — | — | N | crash-path buffer |
| Core/Base/ModuleTable.hpp:62,71 | `name[64]`, `kMax` | 64, 512 | CONSTANT | — | — | — | — | — | — | N | static crash-path module table |
| Core/Base/ForeignModules.cpp:34-79 / .hpp:66-68 | catalogue strings, `kTable`, tiers | data | CONSTANT | — | — | — | — | — | — | N | catalogue data, not tunables |
| Core/Base/Engine.cpp:63 | wide-buffer cap | 65536 (wchar) | CONSTANT | — | — | — | — | — | — | N | Win32 path API sanity bound |
| Core/Base/Log.hpp:77-78 | `kBacklogLines`, `kBacklogLineBytes` | 512, 512 | CONSTANT | — | — | — | — | — | — | N | static lock-free crash-path ring (could become Restart only if heap-sized at Init) |
| Core/Base/Log.cpp:230 | engine sink on stderr | stderr | CONSTANT | — | — | — | — | — | — | N | stdout is the Hub's data channel |
| Core/Base/Log.cpp:232 | console pattern | `%^[%H:%M:%S.%e] [%n] [%l]%$ %v` | CONSTANT | — | — | — | — | — | — | N | the editor Console parses the prefix: Panels/ConsoleModel.cpp:18, :48 (R2) |
| Core/Base/Log.cpp:329 | rotation `keep` | 5 (files) | SETTING | log.file.keepCount | LogFileSettings | Game Dev | Pref-P | Restart | [0,100] | N | retention |
| Core/Base/Log.cpp:357 | `truncate` = true | bool | DERIVED | — | — | — | — | — | — | N | paired with the rename rotation |
| Core/Base/Log.cpp:368 | `flush_on(warn)` | warn | SETTING | log.file.flushLevel | LogFileSettings | Game Dev | Pref-P | Live | [0,6] | N | I/O versus durability |
| Core/Util/Logger.hpp:19 | `SPDLOG_ACTIVE_LEVEL` | TRACE | CONSTANT | — | — | — | — | — | — | N | compile-time strip level; the runtime knob is the level cvar |
| Core/Util/Logger.hpp:56 | `Init(consoleLevel=Info, fileLevel=Trace)` | Info / Trace | SETTING | log.server.consoleLevel / log.server.fileLevel | ServerLogSettings | Server | Project | Live | [0,6] | N | server ops |
| Core/Util/Logger.hpp:205 | console-sink pattern | string | SETTING | log.server.pattern | ServerLogSettings | Server Dev | Project | Live | string | N | format preference (S6-45 amendment): the console sink's pattern; split from the file pattern, one name per value |
| Core/Util/Logger.hpp:244 | file-sink pattern | string | SETTING | log.server.filePattern | ServerLogSettings | Server Dev | Project | Live | string | N | format preference (S6-45 amendment): registered by S6-3; split from log.server.pattern |
| Core/Util/Logger.hpp:221 | `flush_on(info)` | info | SETTING | log.server.flushLevel | ServerLogSettings | Server | Project | Live | [0,6] | N | I/O versus durability |
| Core/Util/Logger.hpp:241-242 | rotating sink | 5 MiB, 3 files | SETTING | log.server.file.maxBytes / log.server.file.maxFiles | LogServerFileSettings | Server | Project | Restart | [64 KiB,1 GiB] / [1,100] | N | retention |

### Assets

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Assets/Assets.hpp:71 | `AssetsDesc::byteBudget` | 256 MiB | SETTING | assets.cache.byteBudget | AssetsCacheSettings | Game | Project | Restart | [0=unbounded, 16 GiB] | N | memory budget; bound at Runtime.cpp:220 `Assets::Create()` |
| Core/Assets/Assets.cpp:1015 | material parent-chain depth | 8 | SETTING | assets.material.maxParentDepth | AssetsSettings | Editor Dev | Project | Live | [1,64] | N | cycle-guard bound; when in doubt |
| Core/Assets/Assets.hpp:503, :532 | `LoadDisplayPixels maxSize`, `WriteThumbnailPngRgba maxWidth=0` | params | DERIVED | — | — | — | — | — | — | N | caller supplies; the thumbnail size lives at the editor caller (golden-bound) |
| Core/Assets/Assets.cpp:1078-1096 | `kLeaf`, `kOpaque`, `kSource` extension lists | strings | CONSTANT | — | — | — | — | — | — | N | decoder capability; mirrored at AssetRegistry.cpp:36/185 |
| Core/Assets/Assets.cpp:112-116, :1196 | GUID string shape 36; manifest `version >= 4` | 36, 4 | CONSTANT | — | — | — | — | — | — | N | format |
| Core/Assets/Assets.cpp:1513, :1634, :1573 | stbi forced 4 channels; alpha 255 | 4, 255 | CONSTANT | — | — | — | — | — | — | N | pixel contract |
| Core/Assets/ArtifactReader.cpp:15-37, 243, 480, 614, 871-874 | artifact magic/version/entry sizes, `kHeaderProbeBytes`, vertex stride, FNV prime, GLB constants | various | CONSTANT | — | — | — | — | — | — | N | file format |
| Core/Assets/ArtifactReader.hpp:287, :330 | importer version mirrors | 1, 1 | CONSTANT | — | — | — | — | — | — | N | cook/format version |
| Core/Assets/ImageCompare.cpp:39-40, 88-92, 264-266, 278-280, 292, 359 | dE94/SSIM/Lab constants, 31x31 window, 0.99, 10x10 grid | various | CONSTANT | — | — | — | — | — | — | N | ported Playwright algorithm; golden oracle |
| Core/Assets/ImageCompare.hpp:96-97, :168, :215 | padding colours, `maxColorDeltaE94` | 1.0 | CONSTANT | — | — | — | — | — | — | N | JND, "DERIVED, not tuned" (comment); test oracle |

### Project

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Project/ProjectManifest.hpp:69 | `kFormatVersion` | 2 | CONSTANT | — | — | — | — | — | — | N | file format |
| Core/Project/ProjectManifest.hpp:38 | `SplashConfig::enabled` | true | SETTING | app.splash.enabled | AppSplashSettings | Game | Project | Restart | bool | N | already .arcproj data (second store) (reconciled R1) |
| Core/Project/ProjectManifest.hpp:39 | `SplashConfig::image` | "" -> engine branding | SETTING | app.splash.image | AppSplashSettings | Game | Project | Restart | asset path | N | branding choice (reconciled R1) |
| Core/Project/ProjectManifest.hpp:40 | `backgroundColor` | (0.05,0.05,0.06) | SETTING | app.splash.backgroundColor | AppSplashSettings | Game | Project | Restart | colour | N | colour (reconciled R1) |
| Core/Project/ProjectManifest.hpp:48 | `showProgress` | false | SETTING | app.splash.showProgress | AppSplashSettings | Game | Project | Restart | bool | N | toggle (reconciled R1) |
| Core/Project/ProjectManifest.hpp:49 | `minDurationSeconds` | 0 (s) | SETTING | app.splash.minDurationSeconds | AppSplashSettings | Game | Project | Restart | [0,10] | N | timing (reconciled R1) |
| Core/Project/ProjectManifest.hpp:58 | `PhysicsConfig::gravity` | (0,-9.81) m/s^2 | SETTING | physics.gravity | Physics2DWorldSettings | Game | Project | NextWorld | each axis [-1000,1000] | Y | already .arcproj data; per-scene override exists (R2: "Live via world rebuild" is NextWorld until Manifold2D gains a setter) |
| Core/Project/Project.cpp:428 | stamped `{0.0,-9.81}` | m/s^2 | DERIVED | — | — | — | — | — | — | N | shadow copy of the PhysicsConfig default |
| Core/Project/Project.cpp:408, :443-457 | folder set, `.gitignore` template | strings | CONSTANT | — | — | — | — | — | — | N | project layout contract |
| Core/Project/Project.cpp:279, :494, :624; Core/Base/Runtime.cpp:224, :482 | Saved/Diagnostics, Saved/editor.lock, data/EngineConfig, Saved/Config | paths | CONSTANT | — | — | — | — | — | — | N | rung/layout locations (spec s11.1) |
| Core/Project/ProjectOpenOptions.hpp:51 | `mountDiagnostics` | true | SETTING | editor.assets.mountDiagnostics | EditorAssetsSettings | Editor Dev | Pref-P | NextWorld (next project open) | bool | N | per-call toggle (and an ABI-sensitive struct, see :20-37) |
| Core/Project/AssetRegistry.cpp:36 | `kBinaryExts` | strings | CONSTANT | — | — | — | — | — | — | N | importer capability (duplicates Assets.cpp:1078 kLeaf) |
| Core/Project/AssetRegistry.cpp:185 | `kSourceExts` | strings | CONSTANT | — | — | — | — | — | — | N | duplicated verbatim at Assets.cpp:1095 |
| Core/Project/AssetRegistry.cpp:197 | `kSourceIdNamespace` | GUID | CONSTANT | — | — | — | — | — | — | N | stable identity namespace |
| Core/Project/AssetRegistry.cpp:162 | sidecar `version` | 1 | CONSTANT | — | — | — | — | — | — | N | file format |

### Scene / Physics

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Scene/Components.hpp:213 | `PhysicsSettings::gravity` (component) | (0,-9.81) | DERIVED | — | — | — | — | — | — | N | per-scene override; should seed from physics.gravity when added |
| Core/Base/Runtime.cpp:429 | `ProjectManifest::PhysicsConfig{}.gravity` fallback | (0,-9.81) | DERIVED | — | — | — | — | — | — | N | no-project fallback = physics.gravity default |
| Core/Scene/Physics2D.cpp:19, :22, :46 | floor-normal threshold | 0.5 (normal.y, about 60 deg slope) | SETTING | physics.ground.minNormalY | PhysicsGroundSettings | Game | Project | Live | [0,1] | Y | game feel (walkable slope) |
| Core/Scene/Physics2D.cpp:44 | ground probe reach | 0.05 (m) | SETTING | physics.ground.probeDistance | PhysicsGroundSettings | Game | Project | Live | [0,1] | Y | game feel (coyote reach) |
| Core/Scene/PhysicsSystem.hpp | hit event threshold | 1.0 (m/s) | SETTING | physics.events.hitThreshold | PhysicsEventSettings | Game | Project | Live | [0,100] | Y | approach speed an impact must exceed to report a hit (spec 2026-10-08 s7.5) |
| Core/Scene/PhysicsSystem.hpp:207-208 | `kAuthorPosEps`, `kAuthorRotEps` | 1e-5 m / rad | CONSTANT | — | — | — | — | — | — | N | numeric round-trip noise tolerance |
| Core/Scene/PhysicsSystem.hpp:213-214; SceneResources.hpp:52-53 | `kPi`, `kTau` | pi, 2pi | CONSTANT | — | — | — | — | — | — | N | math identity |
| Core/Scene/PhysicsSystem.hpp:696; Runtime.cpp:418, :461 | `m_fixedDt` = 1/fixedHz | s | DERIVED | — | — | — | — | — | — | N | from sim.fixedHz, captured at AddSystem |
| Core/Scene/PhysicsComponents.hpp:73-78 | RigidBody2D defaults (Kinematic, mass 0, damping 0, fixedRotation/bullet false) | - | CONSTANT | — | — | — | — | — | — | N | authored-data default; an absent JSON key resolves to it (SceneSerializer.hpp:384) |
| Core/Scene/PhysicsComponents.hpp:103-115 | Fixture defaults radius 0.5, halfW/H 0.5, density 1, friction 0.3, restitution 0 | m, kg/m^2 | CONSTANT | — | — | — | — | — | — | N | authored-data default (absent-key meaning); a "default physics material" is new work |
| Core/Scene/Components.hpp:264-269 | Camera defaults ortho 5, fov 60, near 0.1, far 1000 | m, deg | CONSTANT | — | — | — | — | — | — | N | pre-field scenes rely on them (comment :250-257) |
| Core/Scene/SceneCamera.hpp:210-212 | local fov/near/far initialisers | 60 / 0.1 / 1000 | DERIVED | — | — | — | — | — | — | N | dead mirror of the Camera defaults (overwritten or nullopt) |
| Core/Scene/Components.hpp:383, :386 | sortingLayer / orderInLayer Range | 0..65535 | CONSTANT | — | — | — | — | — | — | N | uint16 storage |
| Core/Scene/Components.hpp:436, :444, :448, :452 | Camera Ranges ortho [0.01,1e4], fov [1,179], near [0.001,1e4], far [0.01,1e6] | m, deg | CONSTANT | — | — | — | — | — | — | N | schema/inspector validation; fov bound is math |
| Core/Scene/ViewTransform.hpp:130 | `Orthographic(nearZ=-1000, farZ=1000)` | +/-1000 (m) | SETTING | render.ortho2D.depthRange | RenderViewSettings | Game Dev | Project | Live | [1,1e6] | N | sprites beyond +/-1 km Z clip (call at SceneCamera.hpp:109) |
| Core/Scene/ViewTransform.hpp:70, :120; SceneCamera.hpp:251 | epsilons | 1e-12, 1e-6 | CONSTANT | — | — | — | — | — | — | N | float tolerance |
| Core/Scene/BoundsSystem.hpp:45 | `kSpriteDepthEpsilon` | 0.001 (m) | CONSTANT | — | — | — | — | — | — | N | degenerate-frustum guard; editor goldens depend on it |
| Core/Scene/SceneResources.hpp:128-131 | resolved sprite uv/size/pivot | 0..1, 1 m, 0.5 | DERIVED | — | — | — | — | — | — | N | resolution outputs |
| Core/Scene/SceneResources.hpp:227-230 | rings 16 / segments 32 / subdiv 1 / capsule 2 | - | DERIVED | — | — | — | — | — | — | N | mirror of MeshAssetData for cache identity |
| Core/Scene/SceneResources.hpp:288-291 | ResolvedMeshMaterial baseColor 1, alphaCutoff 0.5, twoSided false | - | CONSTANT | — | — | — | — | — | — | N | resolution of an .arcmat with absent keys |
| Core/Scene/TransformSystems.hpp:79 | `kNoParent` | 0xFFFFFFFF | CONSTANT | — | — | — | — | — | — | N | sentinel |
| Core/Scene/TransformSystems.hpp:265 | `visited.Reserve(64)` | 64 | CONSTANT | — | — | — | — | — | — | N | allocation hint, no observable effect |
| Core/Scene/Frustum.hpp:39-53 | 6 planes, normalisation | - | CONSTANT | — | — | — | — | — | — | N | math |

### Sim

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Sim/RunLoop.hpp:39 | `Config::fixedHz` | 60 (Hz) | SETTING | sim.fixedHz | SimSettings | Game | Project | NextWorld | [10,480] | Y | fixed step; see Notes on the SetFixedHz desync |
| Core/Sim/RunLoop.hpp:40 | `maxStepsPerFrame` | 5 (steps) | SETTING | sim.maxStepsPerFrame | SimSettings | Game | Project | Live | [1,64] | Y | spiral-of-death clamp; changes the steps per hitch |
| Core/Sim/RunLoop.hpp:255; Time.hpp:34 | `m_timeScale` / `timeScale` | 1.0 | CONSTANT | — | — | — | — | — | — | N | identity default of a runtime control (SetTimeScale), not stored |

### Jobs

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Jobs/JobSystem.hpp:29 (constructed default at Runtime.cpp:97, :126) | `JobSystem(threads = 0)` | 0 = hardware threads | SETTING | jobs.workerThreads | JobsSettings | Game | Pref-P | Restart | [0=auto,256] | N | per machine; thread-count invariance is tested |
| Core/Jobs/JobSystem.cpp:104 | `threads - 1` | - | DERIVED | — | — | — | — | — | — | N | enki total-versus-workers |
| Core/Jobs/JobSystem.cpp:43 | `m_MinRange = minBatch or 1` | - | DERIVED | — | — | — | — | — | — | N | call-site batch |
| Core/Jobs/JobSystem.cpp:105-106 | `threadStart` stack guarantee | hook | CONSTANT | — | — | — | — | — | — | N | crash-path contract |

### Net (Server)

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Net/TcpSocket.hpp:48 | `MAX_PAYLOAD_SIZE` | 8192 (B) | SETTING | net.maxPayloadBytes | NetServerSettings | Server | Project | Restart | [1 KiB,1 MiB] | N | overlaps protocol.json `max_message_size` |
| Core/Net/TcpSocket.hpp:49 (and :432 default arg) | `MAX_RECEIVE_BUFFER_SIZE` | 65536 (B) | SETTING | net.maxReceiveBufferBytes | NetServerSettings | Server | Project | Restart | [4 KiB,16 MiB] | N | DoS budget |
| Core/Net/TcpSocket.hpp:50 | `RECV_CHUNK_SIZE` | 4096 (B) | CONSTANT | — | — | — | — | — | — | N | capacity hint (R2): a stack-array bound in the Aphelyon services (ServiceClient.hpp:229); ARC_CONSTANT marker |
| Core/Net/TcpSocket.hpp:51 | `RECV_TIMEOUT_MS` | 100 (ms) | SETTING | net.recvTimeoutMs | NetServerSettings | Server | Project | Restart | [1,10000] | N | timeout |
| Core/Net/TcpSocket.hpp:80 | `MAX_CONNECTIONS_TOTAL` | 2048 | SETTING | net.maxConnectionsTotal | NetServerSettings | Server | Project | Restart | [1,1e6] | N | fallback for protocol.json key (Protocol.hpp:184) |
| Core/Net/TcpSocket.hpp:81 | `MAX_CONNECTIONS_PER_IP` | 16 | SETTING | net.maxConnectionsPerIp | NetServerSettings | Server | Project | Restart | [1,10000] | N | topology-dependent (comment :66-77) |
| Core/Net/TcpSocket.hpp:115-117 | keepalive idle / interval / probes | 120 s / 30 s / 8 | SETTING | net.keepalive.idleSeconds / .intervalSeconds / .probeCount | NetKeepaliveSettings | Server | Project | Restart | [1,7200] / [1,600] / [1,32] | N | network-environment tuning |
| Core/Net/TcpSocket.hpp:234 | `listen(sock, 10)` backlog | 10 | SETTING | net.listenBacklog | NetServerSettings | Server Dev | Project | Restart | [1,SOMAXCONN] | N | burst-accept tuning |
| Core/Net/TcpSocket.hpp:440 | `buffer.size() <= 10` | 10 (digits) | CONSTANT | — | — | — | — | — | — | N | wire framing: max length-prefix width |
| Core/Net/RateLimiter.hpp:47 | `MAX_RECORDS` | 10000 | SETTING | net.rateLimit.maxRecords | NetRateLimitSettings | Server | Project | Restart | [100,1e7] | N | memory/LRU cap |
| Core/Net/RateLimiter.hpp:51-53 | `Config` maxAttempts / window / cooldown | 5 / 60 s / 30 s | SETTING | net.rateLimit.maxAttempts / .windowSeconds / .cooldownSeconds | NetRateLimitSettings | Server | Project | Live | [1,1000] / [1,86400] / [0,86400] | N | per-call struct; bind at the server call sites |
| Core/Net/RateLimiter.hpp:162 | cleanup every N calls | 100 | SETTING | net.rateLimit.cleanupEvery | NetRateLimitSettings | Server Dev | Project | Live | [1,1e6] | N | when in doubt |
| Core/Net/RateLimiter.hpp:166 | idle expiry | 10 (min) | SETTING | net.rateLimit.idleExpiryMinutes | NetRateLimitSettings | Server | Project | Live | [1,1440] | N | retention |
| Core/Net/Protocol.hpp:116 | `Load(path = "data/protocol.json")` | path | SETTING | net.protocolPath | NetServerSettings | Server Dev | Project | Restart | path | N | path choice |
| Core/Net/Protocol.hpp (protocol.json settings) | default_port | 0 | SETTING | net.defaultPort | NetServerSettings | Server | Project | Restart | 0..65535 | N | (S6-45 amendment): a service's public TCP port when -p is absent (S6-12) |
| Core/Net/Protocol.hpp (protocol.json settings) | token_length | 64 | SETTING | net.tokenLength | NetServerSettings | Server | Project | Restart | 1..1024 | N | (S6-45 amendment): the session token length; Message::HasToken's minimum (S6-12) |
| Core/Net/Protocol.hpp (protocol.json settings) | session_lifetime | 0 (s) | SETTING | net.sessionLifetimeSeconds | NetServerSettings | Server | Project | Restart | 0..31536000 | N | (S6-45 amendment) (S6-12) |
| Core/Net/Protocol.hpp (protocol.json settings) | idle_timeout | 0 (s) | SETTING | net.idleTimeoutSeconds | NetServerSettings | Server | Project | Restart | 0..31536000 | N | (S6-45 amendment) (S6-12) |
| Core/Net/Protocol.hpp (protocol.json settings) | heartbeat_interval | 0 (s) | SETTING | net.heartbeatIntervalSeconds | NetServerSettings | Server | Project | Restart | 0..86400 | N | (S6-45 amendment) (S6-12) |
| Core/Net/Protocol.hpp:23 | `kInvalidMsgId` | 0 | CONSTANT | — | — | — | — | — | — | N | wire sentinel |
| Core/Net/Protocol.hpp:213 | id range | 1..65535 | CONSTANT | — | — | — | — | — | — | N | uint16 wire id |
| Core/Net/Protocol.hpp:410-411 | token log preview 8 + 4 chars | chars | CONSTANT | — | — | — | — | — | — | N | widening leaks session tokens into logs |
| Core/Net/Protocol.hpp:413 | payload log preview | 100 (chars) | CONSTANT | — | — | — | — | — | — | N | security: Login payloads carry passwords (see Notes) |
| Core/Net/Protocol.hpp:418 | `token.length() >= 64` | 64 | CONSTANT | — | — | — | — | — | — | N | wire contract (duplicates protocol.json token_length) |
| Core/Net/Protocol.hpp:426-427 | request < 100 <= response | 100 | CONSTANT | — | — | — | — | — | — | N | wire id partition |

### Remote (Servitor P0)

Path prefix: `Remote/` = `Source/Runtime/ArcaneRemote/src/Arcane/Remote/`. Each default is the one spelling in `ArcaneRemoteProtocol`'s `Protocol.hpp` (`kFrameBudgetMs`, `kQueueDepthDefault`, ARC_CONSTANT-marked). The editor's `editor.remote.enable` is in Part 3's "Existing cvars (e)" table.

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Remote/Dispatcher.cpp | remote frame budget | 4.0 (ms) | SETTING | remote.server.frameBudgetMs | (ARC_CVAR remote.server) | Server Dev | Project | Live | (0,max] | N | milliseconds of remote commands drained before the next tick; read by each Drain whose argument is not positive |
| Remote/Dispatcher.cpp | remote request queue depth | 64 (requests) | SETTING | remote.server.queueDepth | (ARC_CVAR remote.server) | Server Dev | Project | Restart | [1,max] | N | remote requests a host queues before it answers host.busy queue-full; read when the host sizes its RequestQueue |

### Crypto

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Crypto/Crypto.hpp:52 | `DEFAULT_ITERATIONS` | 200000 (PBKDF2 rounds) | SETTING | crypto.pbkdf2Iterations | CryptoSettings | Server | Project | Live | [200000 floor, 5000000] | N | raise-only; the floor stops it weakening; lazy rehash rolls it out (comment :45-50 says bump before release) |
| Core/Crypto/Crypto.hpp:53 | `SALT_LENGTH` | 16 (B) | CONSTANT | — | — | — | — | — | — | N | security parameter |
| Core/Crypto/Crypto.hpp:54 | `HASH_LENGTH` | 32 (B) | CONSTANT | — | — | — | — | — | — | N | SHA-256 output size |
| Core/Crypto/Crypto.hpp:178 | `ENTROPY_SAMPLE_BYTES` | 32 (B) | CONSTANT | — | — | — | — | — | — | N | RNG self-test parameter |
| Core/Crypto/Crypto.hpp:384 | `BLOCK_SIZE` | 64 (B) | CONSTANT | — | — | — | — | — | — | N | SHA-256 block |
| Core/Guid.cpp:41-42, :100, :119; Guid.hpp:58 | version/variant bits, v4/v5, hash mix | - | CONSTANT | — | — | — | — | — | — | N | RFC 4122 / math |

### Plugin

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Plugin/PluginHost.cpp:1003 | reload settle window | 250 (ms) | SETTING | plugin.hotReload.settleMs | PluginHotReloadSettings | Editor Dev | Pref-P | Live | [0,5000] | N | debounce for slow linkers/AV scanners |
| Core/Plugin/PluginHost.cpp:865, :869 | copy retries | 5 x 50 ms | SETTING | plugin.hotReload.copyRetries / .copyRetryMs | PluginHotReloadSettings | Editor Dev | Pref-P | Live | [1,50] / [1,1000] | N | file-lock contention |
| Core/Plugin/PluginHost.cpp:293, :304 | `<stem>_<gen>.dll/.pdb` | name | CONSTANT | — | — | — | — | — | — | N | versioned-image naming |
| Core/Plugin/PluginABI.hpp:1043 | `kGamePluginABIVersion` | 51 | CONSTANT | — | — | — | — | — | — | N | ABI |
| Core/Plugin/PluginABI.hpp:1084-1094 | entry-point names | strings | CONSTANT | — | — | — | — | — | — | N | ABI |
| Core/Plugin/Plugin.cpp:98-100 | `hostDebugCrt` | build flag | CONSTANT | — | — | — | — | — | — | N | build configuration |
| Core/Plugin/Module.cpp:51-56, :102, :243, :258 | CRT import names, buf 512, PE scan 4096/256 | - | CONSTANT | — | — | — | — | — | — | N | PE format bounds |

### Build / Platform / Misc

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Build/Toolchain.cpp:214-217, :229 | premake5 lookup (SDK ThirdParty, then PATH) | path | SETTING | build.premakePath | BuildToolSettings | Editor Dev | Pref-P | Live | path (empty = discover) | N | override for non-standard installs |
| Core/Build/Toolchain.cpp:242, :276, :281 | vswhere `-latest -requires ...MSBuild` | query | SETTING | build.msbuildPath | BuildToolSettings | Editor | Pref-P | Live | path (empty = discover) | N | "-latest" picks the newest VS; pinning is a preference |
| Core/Build/Toolchain.cpp:292-296 | prefer `mingw32-make` over `make` | choice | SETTING | build.makePath | BuildToolSettings | Editor Dev | Pref-P | Live | path | N | tool choice |
| Core/Build/Toolchain.cpp:303-316, :327 | ninja / xcodebuild `/usr/bin` / devenv query | paths | SETTING | build.ninjaPath / build.ideExecutable | BuildToolSettings | Editor Dev | Pref-P | Live | path | N | tool choice |
| Core/Build/Toolchain.cpp:54-56 | `kPathListSep` | ';' / ':' | CONSTANT | — | — | — | — | — | — | N | OS |
| Core/Platform/NativeWindow.hpp:11-21 | NativeWindowDesc defaults 480x270, popup, colour 0x0D0D0F | - | DERIVED | — | — | — | — | — | — | N | per-call desc; the splash/reporter callers (ArcaneClient) own the values |
| Core/Platform/NativeWindow.cpp:33-34, :195, :236, :279 | WM_APP ids, Sleep(2), 96 DPI | - | CONSTANT | — | — | — | — | — | — | N | Win32 / OS baseline DPI |
| Core/Cli/Cli.cpp:161; Project/MountTable.cpp:22 | "--" / "://" parsing | - | CONSTANT | — | — | — | — | — | — | N | syntax |
| Core/Version.hpp:8-11 | version 0.1 "M5" | - | CONSTANT | — | — | — | — | — | — | N | identity |

### Serialization

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Serialization/SceneSerializer.hpp:97, :111 | `kSceneJsonVersion`, `kSceneJsonVersionMin` | 6, 3 | CONSTANT | — | — | — | — | — | — | N | file format |
| Core/Serialization/SceneAsset.hpp:42 | `kSceneExt` | ".arcscene" | CONSTANT | — | — | — | — | — | — | N | file format |
| Core/Serialization/RegistrySnapshot.hpp:37-41 | magic / version / header size | 0x53535241, 1, 10 | CONSTANT | — | — | — | — | — | — | N | blob format |
| Core/Serialization/ReflectionJson.hpp:251 | `kQuatNormTolerance2` | 0.05 | CONSTANT | — | — | — | — | — | — | N | file acceptance must not vary per machine |
| Core/Serialization/ReflectionJson.hpp:164-209, :319 | vec/mat element counts, 8-byte enum memcpy | - | CONSTANT | — | — | — | — | — | — | N | type layout |

### Math

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Math/Aabb.hpp:26, :46-66; NormalMatrix.hpp:63-66 | inf, 0.5, 8 corners, identity | - | CONSTANT | — | — | — | — | — | — | N | math identity |

### Mesh / Material / Sprite

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Core/Mesh/MeshAsset.hpp:96-98, :105 | rings 16, segments 32, subdivisions 1, capsuleLengthRatio 2 | count, ratio | CONSTANT | — | — | — | — | — | — | N | absent-key default (MeshAsset.cpp:167-172); the per-asset value is the tunable |
| Core/Mesh/MeshAsset.cpp:233-257 | rings/segments >= 3, ratio >= 1 | - | CONSTANT | — | — | — | — | — | — | N | geometric minimum |
| Core/Mesh/MeshAsset.cpp:287-288; MeshBuilder.cpp:46-53, :310-311 | unit cube 1.0, sphere/cylinder radius 0.5, half-height 0.5 | m | CONSTANT | — | — | — | — | — | — | N | unit-primitive contract (scale via Transform); thumbnail goldens |
| Core/Mesh/MeshAsset.cpp:373-379 | vertex stride 8 floats | - | CONSTANT | — | — | — | — | — | — | N | artifact format |
| Core/Sprite/SpriteAsset.hpp:35 | `ppu` | 100 (px/m) | SETTING | assets.sprite.defaultPixelsPerUnit | AssetsSpriteSettings | Editor | Project | Live | [1,10000] | N | seeds new sprites (Unity's default PPU); keep the struct default as the absent-key fallback |
| Core/Sprite/SpriteAsset.cpp:108 | `ppu > 0 ? ppu : 100` | 100 | DERIVED | — | — | — | — | — | — | N | shadow copy of the struct default |
| Core/Sprite/SpriteAsset.hpp:36-37, :45 | sourcePos/Size 0, pivot 0.5 | - | CONSTANT | — | — | — | — | — | — | N | sparse write (SpriteAsset.cpp:24-29): absent key = this default |
| Core/Material/GlobalParams.hpp:16-25 | cbuffer/texture slots | b0/b1/b2, t1 | CONSTANT | — | — | — | — | — | — | N | shader contract |
| Core/Material/MaterialSource.hpp:88 | `kMaxPassInputs` | 4 | CONSTANT | — | — | — | — | — | — | N | shader contract (reserved InputTexture..3) |
| Core/Material/MaterialSource.hpp:94; MaterialTypes.hpp:147 | `kSceneInput`, `kNoSlot` | 0xFFFFFFFF | CONSTANT | — | — | — | — | — | — | N | sentinels |
| Core/Material/MaterialSource.cpp:14, :20, :29 | `//@param`, reserved names, vertex passthrough | text | CONSTANT | — | — | — | — | — | — | N | source format / shader template |
| Core/Material/MaterialSource.cpp:126, :476, :542; MaterialTypes.hpp:54, :58 | FNV-1a constants | - | CONSTANT | — | — | — | — | — | — | N | hash identity (cache keys) |
| Core/Material/MaterialTemplate.cpp:13 | `kRegister` | 16 (B) | CONSTANT | — | — | — | — | — | — | N | HLSL cbuffer packing |
| Core/Material/MaterialTypes.hpp:137-138 | `sliderMin/Max` | 0 / 1 | CONSTANT | — | — | — | — | — | — | N | absent-@range default |
| Core/Material/MaterialAsset.cpp:336 | alphaCutoff in [0,1] | - | CONSTANT | — | — | — | — | — | — | N | validation |
| Core/Material/MaterialGraph.cpp:23-47, :306, :1466-1497 | pin tables, reserved names, neutral values (Noise scale 10 at :1483) | - | CONSTANT | — | — | — | — | — | — | N | graph semantics: saved graphs with unwired pins depend on them |
| Core/Material/MaterialGraph.hpp:268 | `customOutWidth` | 4 | CONSTANT | — | — | — | — | — | — | N | graph format default |

### Library configs (where Arcane creates the object; the fields it leaves at the library default)

Creation sites:
- Astra `Registry::Config`: Core/Base/Runtime.cpp:214-216 (ctor), :348-350 (RestoreRegistry), :378-380 (ResetRegistry) and Core/Sim/Simulation.hpp:29-33. All four set only `workScheduler`.
- Manifold2D `WorldDef`: Core/Base/Runtime.cpp:452-455. Sets only `gravityX/Y`.
- enkiTS `TaskSchedulerConfig`: Core/Jobs/JobSystem.cpp:102-107.
- Astra `Registry::Save()`: Core/Base/Runtime.cpp:318, with default `SaveConfig`.
- Mosaic: Core/Base/Log.cpp:248-249 sets only the sink and the assert handler.

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| TP/Astra/include/Astra/Archetype/ArchetypeChunkPool.hpp:860 (:847) | `chunkSize` | 16 KiB | SETTING | astra.memory.chunkSize | AstraMemorySettings | Game Dev | Project | NextWorld | [4 KiB,1 MiB] | N | cache tuning; left at default |
| ArchetypeChunkPool.hpp:861 | `chunksPerBlock` | 128 | SETTING | astra.memory.chunksPerBlock | AstraMemorySettings | Game Dev | Project | NextWorld | [1,4096] | N | left at default |
| ArchetypeChunkPool.hpp:862 | `maxChunks` | 4096 | SETTING | astra.memory.maxChunks | AstraMemorySettings | Game | Project | NextWorld | [64,1e7] | N | hard ceiling on world size; left at default |
| ArchetypeChunkPool.hpp:863 | `initialBlocks` | 0 | SETTING | astra.memory.initialBlocks | AstraMemorySettings | Game Dev | Project | NextWorld | [0,1024] | N | prewarm; left at default |
| ArchetypeChunkPool.hpp:864 | `useHugePages` | true | SETTING | astra.memory.chunkHugePages | AstraMemorySettings | Game Dev | Project | NextWorld | bool | N | OS-permission dependent; left at default |
| ArchetypeChunkPool.hpp:869-871 | `minChunkBytes` / `maxChunkBytes` / `growDivisor` | 4 KiB / 512 KiB / 2 | SETTING | astra.memory.minChunkBytes / .maxChunkBytes / .growDivisor | AstraMemorySettings | Game Dev | Project | NextWorld | [4 KiB, maxChunkBytes] / [minChunkBytes, 1 MiB] / [1,64] | N | grow policy; left at default |
| TP/Astra/include/Astra/Entity/EntityTable.hpp:32 (via EntityManager.hpp:27-34) | `entitiesPerSegment` | 65536 | SETTING | astra.memory.entitiesPerSegment | AstraMemorySettings | Game Dev | Project | NextWorld | pow2 [1024,65536] | N | left at default |
| EntityTable.hpp:35 | `releaseThreshold` | 0.1 | SETTING | astra.memory.entityReleaseThreshold | AstraMemorySettings | Game Dev | Project | NextWorld | [0,1] | N | serialized (EntityManager.hpp:335) but no consumer found in Astra (see Notes) |
| EntityTable.hpp:36-38 | `autoRelease` / `maxEmptySegments` / `maxPooledSegments` | true / 2 / 4 | SETTING | astra.memory.entityAutoRelease / .maxEmptySegments / .maxPooledSegments | AstraMemorySettings | Game Dev | Project | NextWorld | bool / [0,64] / [0,64] | N | left at default |
| EntityTable.hpp:39 | `useHugePages` | true | SETTING | astra.memory.entityHugePages | AstraMemorySettings | Game Dev | Project | NextWorld | bool | N | left at default |
| TP/Astra/include/Astra/Component/ResourceStorage.hpp:51 | `initialResourceCapacity` | 32 | SETTING | astra.memory.initialResourceCapacity | AstraMemorySettings | Game Dev | Project | NextWorld | [0,4096] | N | when in doubt; left at default |
| TP/Astra/include/Astra/Registry/Registry.hpp:49 | `workScheduler` | null -> JobSystem adapter | DERIVED | — | — | — | — | — | — | N | set from JobSystem::WorkScheduler() |
| TP/Astra/include/Astra/Registry/Registry.hpp:1716-1718 | `SaveConfig` LZ4 / Fast / 1024 B | - | SETTING | astra.snapshot.compression | AstraSnapshotSettings | Editor Dev | Pref-P | Live | enum{None,LZ4} | N | hot-reload snapshot speed versus size; left at default |
| TP/Manifold2D/include/Manifold2D/Physics/PhysicsWorld.hpp:175 | `broadphase` | Tree | SETTING | physics.broadphase | Physics2DWorldSettings | Game | Project | NextWorld | enum{Tree,Grid...} | Y | left at default |
| PhysicsWorld.hpp:176 | `hashCellSize` | 1 (m) | SETTING | physics.hashCellSize | Physics2DWorldSettings | Game Dev | Project | NextWorld | [0.05,100] | Y | grid broadphase only; left at default |
| PhysicsWorld.hpp:177-179 | `passability`, `tileCellSize` 1, `tileOrigin` 0 | - | DERIVED | — | — | — | — | — | — | N | game-supplied tile seam; meaningful only with passability |
| PhysicsWorld.hpp:188-189 | `gravityX/Y` | lib (0,+10) y-down; Arcane sets them | DERIVED | — | — | — | — | — | — | N | from physics.gravity (Runtime.cpp:453-454) |
| PhysicsWorld.hpp:208 | `substepCount` | 4 | SETTING | physics.substepCount | Physics2DWorldSettings | Game | Project | NextWorld | [1,16] | Y | solver quality; left at default |
| PhysicsWorld.hpp:209 | `contactHertz` | 30 (Hz) | SETTING | physics.contactHertz | Physics2DWorldSettings | Game | Project | NextWorld | [1,240] | Y | left at default |
| PhysicsWorld.hpp:210 | `contactDampingRatio` | 10 | SETTING | physics.contactDampingRatio | Physics2DWorldSettings | Game | Project | NextWorld | [0,100] | Y | left at default |
| PhysicsWorld.hpp:212 | `restitutionThreshold` | 1 (m/s) | SETTING | physics.restitutionThreshold | Physics2DWorldSettings | Game | Project | NextWorld | [0,100] | Y | left at default |
| PhysicsWorld.hpp:214 | `contactPushMaxVelocity` | 3 (m/s) | SETTING | physics.contactPushMaxVelocity | Physics2DWorldSettings | Game | Project | NextWorld | [0,100] | Y | left at default |
| PhysicsWorld.hpp:220 | `maxLinearVelocity` | 400 (m/s) | SETTING | physics.maxLinearVelocity | Physics2DWorldSettings | Game | Project | NextWorld | [1,1e5] | Y | left at default |
| PhysicsWorld.hpp:229 | `sleepThreshold` | 0.05 (m/s) | SETTING | physics.sleepThreshold | Physics2DWorldSettings | Game | Project | NextWorld | [0,10] | Y | left at default |
| PhysicsWorld.hpp:549 | `SetExecutor` (never called) | null -> serial | SETTING | physics.parallelSolver | Physics2DWorldSettings | Game Dev | Project | NextWorld | bool | N | MT-invariance tested; binding missing (Notes) |
| PhysicsWorld.hpp:137 (BodyDef; PhysicsSystem.hpp:514 never sets it) | per-body `sleepThreshold` -1 (inherit). Per-body `eventsEnabled` removed | - | DERIVED | — | — | — | — | — | — | N | sleepThreshold still inherits WorldDef (the BodyDef fill never assigns it). The per-body `eventsEnabled` gate is removed (spec 2026-10-08 s6.2); events are the per-fixture flags `Fixture::contactEvents` / `sensorEvents` / `hitEvents` (PhysicsComponents.hpp, spec s7.4) and the `physics.events.*` cvars (`PhysicsEventSettings`, spec s7.5) |
| TP/Manifold2D/include/Manifold2D/Physics/PhysicsTypes.hpp:146, :150 | `kLinearSlop`, `kMaxRotation` | 0.005 m, pi/4 | CONSTANT | — | — | — | — | — | — | N | compile-time library constants (changing them forks the vendored library) |
| TP/enkiTS/src/TaskScheduler.h:275 | `numTaskThreadsToCreate` | hw-1 when threads == 0 | DERIVED | — | — | — | — | — | — | N | from jobs.workerThreads |
| TaskScheduler.h:282 | `numExternalTaskThreads` | 0 | SETTING | jobs.externalThreads | JobsSettings | Game Dev | Pref-P | Restart | [0,64] | N | needed if non-enki threads submit tasks; left at default |
| TaskScheduler.h:284-286 | other `profilerCallbacks`, `customAllocator` | defaults | CONSTANT | — | — | — | — | — | — | N | wiring for a future Tracy/allocator seam, not a preference |
| TP/Mosaic/include/Mosaic/Log.hpp:100 | `g_logLevel` | Info (never set by Arcane) | DERIVED | — | — | — | — | — | — | N | `log.level` does not reach Astra/Manifold2D logs (Notes) (reconciled R1: one log.level; S2 ApplyLogSettings sets Mosaic::SetLogLevel in every module from it, so no log.mosaicLevel) |

### Should NOT be exposed

- **File-format, artifact and blob constants:** `ArtifactReader.cpp:15-37` (artifact magic, version, entry sizes); `SceneSerializer.hpp:97` `kSceneJsonVersion=6`; `RegistrySnapshot.hpp:37-38` snapshot magic and version. This group also covers authored-data struct defaults that an absent JSON key resolves to: Camera `Components.hpp:264-269`, Fixture `PhysicsComponents.hpp:103-115`, Sprite pivot `SpriteAsset.hpp:45` (sparse write), mesh topology `MeshAsset.hpp:96-105`, ResolvedMeshMaterial `SceneResources.hpp:288-291`.
- **Wire protocol:** `Protocol.hpp:213` uint16 id range; `:426-427` request/response partition at 100; `TcpSocket.hpp:440` length-prefix width.
- **Shader contract and HLSL packing:** `GlobalParams.hpp:16-25` cbuffer/texture slots; `MaterialSource.hpp:88` `kMaxPassInputs=4`; `MaterialTemplate.cpp:13` `kRegister=16`.
- **ABI:** `PluginABI.hpp:1043` `kGamePluginABIVersion`; `:1084-1094` entry names; `CVarTypes.hpp:36-46` flag bits; `Diagnostics.hpp:175-178` exit codes (a monitor/CI contract).
- **Hardware, OS and Win32:** `Diagnostics.cpp:2228` 4 s console-close cap; `:1810` 64 KiB stack guarantee; `NativeWindow.cpp:236` 96 DPI baseline; `Toolchain.cpp:54-56` path separator.
- **Static crash-path capacity** (cannot allocate or read cvars on the crash path): `CrashArena.hpp:46` 256 KiB; `Diagnostics.cpp:225-231` frame/buffer limits; `Log.hpp:77-78` backlog ring; `ModuleTable.hpp:71`.
- **Math identities and float tolerances:** `PhysicsSystem.hpp:213-214` pi/tau; `ViewTransform.hpp:70` 1e-12; `Frustum.hpp`; `Aabb.hpp:26`; `kAuthorPosEps/RotEps`; `kSpriteDepthEpsilon` (also golden-bound).
- **Security and crypto:** `Crypto.hpp:53-54` salt and hash length; `:178` entropy sample; `Protocol.hpp:410-413` log-preview truncation (widening leaks tokens and passwords); RFC 4122 bits in `Guid.cpp`.
- **Test oracle / ported algorithm:** the whole `ImageCompare.*` cascade (dE94 JND, 31x31 SSIM, 0.99, 10x10 grid).
- **Decoder/importer capability lists and layout conventions:** `AssetRegistry.cpp:36/185` and `Assets.cpp:1078-1096` extension lists; `Project.cpp:408` folder set; `Saved/Config`, `Saved/editor.lock`, `data/EngineConfig`; the `.session` name pattern.
- **Vendored-library compile-time constants:** Manifold2D `kLinearSlop`, `kMaxRotation` (`PhysicsTypes.hpp:146-150`).

### Notes

1. **The physics solver never gets the job system.** `PhysicsWorld::SetExecutor` (PhysicsWorld.hpp:549) is called nowhere outside ThirdParty/docs. Runtime::EnsurePhysics (Runtime.cpp:452-455) builds every world serial, even though Runtime owns a JobSystem. The Phase-D1 parallel solver is unreachable in shipped hosts.
2. **Manifold2D has no gravity setter.** The spec s5.2 says "gravity Live (a library setter exists)", but there is no `SetGravity`. Today a gravity change replaces the whole world (Runtime.cpp:444-455) and re-mints all bodies. The binding should declare that honestly, or Manifold2D needs a setter (Manifold2D-first, then re-vendor).
3. **Gravity has at least four stores or copies:**
   - the `.arcproj` `physics.gravity` block (ProjectManifest.hpp:58, a second store outside the cvar registry);
   - the new-project stamp at Project.cpp:428;
   - the per-scene PhysicsSettings component default at Components.hpp:213;
   - the fallback at Runtime.cpp:429.

   The `.arcproj` splash block (ProjectManifest.hpp:36-50) is the same kind of second store. Spec s3.1 says no setting has a second store, so the arc must decide whether the manifest stays as the Project rung or migrates to `Config/<category>.json`.
4. **`sim.fixedHz` cannot be Live today.** `RunLoop::SetFixedHz` (RunLoop.hpp:92) changes the loop step. But PhysicsSystem captured `fixedDt` at AddSystem (Runtime.cpp:418), and `PhysicsEditPass` reads Runtime's own `loopCfg` copy (Runtime.cpp:105, :461). Calling SetFixedHz now desyncs the physics step from the loop. The setting must be NextWorld, or the binding must rebuild PhysicsSystem.
5. **The log level does not reach vendored code.** Arcane never calls `Mosaic::SetLogLevel`. Mosaic's level is a per-module inline atomic defaulting to Info (Mosaic/Log.hpp:100), so the `log.level` cvar never reaches Astra or Manifold2D log output. Each module (Core, Client, host) has its own copy, so the binding must set it in every module, the same way Log.cpp:248 mirrors the sink.
6. **Shadow copies the sweep must delete:**
   - ConsoleModel.cpp:10 (64)
   - Diagnostics.cpp:3032 (60 s)
   - SpriteAsset.cpp:108 (100)
   - SceneCamera.hpp:210-212 (dead initialisers)
   - Project.cpp:428 (gravity)
   - the extension lists duplicated between Assets.cpp:1078/1095 and AssetRegistry.cpp:36/185 (the comment admits "mirrors it exactly")
7. **Second stores on the server side.** `protocol.json` `settings` (Protocol.hpp:168-185) already holds port, max message size, token length, session and idle timeouts, and connection caps. Its code-side fallbacks (`ServerConfig::MAX_CONNECTIONS_*`) and the duplicates (`MAX_PAYLOAD_SIZE` versus `max_message_size`; `HasToken() >= 64` versus `token_length`) make it a second settings store.
8. **Possible security issue: `Message::ToString` logs up to 100 characters of the payload** (Protocol.hpp:413). For Login/Register payloads that likely includes the plaintext password. It is worth checking where ToString is logged. Mosaic's level gap (note 5) aside, this is the only security-relevant finding.
9. **Missing caps and retention, not literals:**
   - `ConsoleModel::m_lines` (ConsoleModel.hpp:45) grows without bound; candidate `console.maxLines`.
   - The diagnostics report directory has no retention or pruning at all; candidate `diagnostics.maxReports`. Engine logs rotate (keep=5), reports do not.
10. **Two logging stacks with different policies.** Engine `Log` uses rename rotation with keep=5, truncate, and flush on warn. Server-facing `Util/Logger.hpp` uses spdlog rotating 5 MiB x 3 and flushes on info. They need two settings families or a unification decision.
11. **Most Core tunables are parameter-struct defaults whose real value is picked by a caller outside Core.** Examples: `Diagnostics::Config`, `AssetsDesc`, `JobSystem(threads)`, `NativeWindowDesc`, `RateLimiter::Config`, `EnableTcpKeepAlive` defaults, the `WriteThumbnailPngRgba` width. The binding belongs at the creation site (the Runtime ctor, host mains, the Aphelyon Server), not in the struct. Changing the struct default would also be an ABI change for the exported ones.
12. **`Sim/Simulation.hpp` is dead code.** Nothing includes it, and it carries a fourth `Registry::Config` construction path. Delete it, or route it through the single `ToAstraConfig` binding.
13. **Astra `maxChunks = 4096` is a hard world-size ceiling** that nobody sets. Also, `EntityTable::Config::releaseThreshold` is serialized (EntityManager.hpp:335) but I found no consumer in Astra, so exposing it would be a placebo until Astra uses it.
14. **Library defaults diverge from engine defaults.** Manifold2D's `BodyDef.friction` is 0.4 versus Arcane's `Fixture.friction` 0.3. `WorldDef` gravity is (0,+10) y-down versus Arcane's (0,-9.81) y-up. Arcane always overrides both, so behaviour is correct, but the binding tests should pin these explicitly.
15. **The PBKDF2 iteration count is flagged in source as "bump before Release launch or 2027"** (Crypto.hpp:45-50). As a floor-clamped Server setting, the rollout is already handled by lazy rehash.
16. **Env-var toggles bypass the cvar store.** `ARCANE_BUILD_MACHINE`, `CI` and `ARCANE_ALLOW_REPORTER_ON_BUILD_MACHINE` (Diagnostics.cpp:2610) toggle reporter spawning. It is worth deciding whether env becomes a documented rung or a `--set` alias.
17. **Crash-path values must be snapshotted at Install.** Any crash-path value converted to a setting (minidump kind, flush timeout, min fatal wait) has to be copied into `g_cfg` at `Install`. The crash thread must never consult the registry.

---

## Part 2: ArcaneClient, ArcaneRuntime, ArcaneServer, ArcaneCrashReporter

### Hard-coded tunables in ArcaneClient, ArcaneRuntime, ArcaneServer and ArcaneCrashReporter (read-only audit, 2026-10-03, against spec sections 3, 5 and 10)

**Conventions used in the tables**
- Paths are relative to `D:\dev\starworks\Arcane\`.
- `—` means the field does not apply. CONSTANT rows always give their reason in the "why" column.
- **Command-line flags:** a flag that is a per-run input, not something anyone would tune (`--project`, `--frames`, `--probe` and so on), is marked CONSTANT with "FLAG ONLY" in the why column. That means "stays a flag, gets no cvar". Rows that say "BOTH" keep the flag and add a cvar.
- **Capacity hints** (reserve sizes, I/O chunk sizes) are marked SETTING Dev because of the "when in doubt" rule. The Notes recommend putting them on the allow-list instead.

### Host command line (HostConfig)
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Host/HostConfig.cpp:85, HostConfig.hpp:16 | backend (`--backend`) | dx12 (enum) | SETTING | render.backend | RenderSettings | PlayerSafe | Project | Restart | dx12 \| vulkan | N | BOTH: keep the flag (scripts, the Hub's saved arguments), add the cvar for a graphics menu |
| HostConfig.cpp:86, HostConfig.hpp:17 | maxFrames (`--frames`) | 0 (frames, 0 = unbounded) | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: automation run budget; a saved value would make every launch quit |
| HostConfig.cpp:87, HostConfig.hpp:18 | vsync (`--no-vsync`) | true (bool) | SETTING | render.vsync | RenderSettings | PlayerSafe | Project | Restart (Live once a swapchain recreate reads it) | bool | N | BOTH: a standard graphics-menu option |
| HostConfig.cpp:88, HostConfig.hpp:19 | perf (`--perf`) | false (bool) | SETTING | diagnostics.perfLog | DiagnosticsSettings | Game | Pref-P | Live | bool | N | BOTH: the console should be able to toggle it mid-session; user 2026-10-06: --perf stays usable in Dist |
| HostConfig.cpp:89, HostConfig.hpp:22 | pluginPath (`--plugin`) | "" | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: overrides the manifest's gameModule, which is where the persisted value lives |
| HostConfig.cpp:90, HostConfig.hpp:23 | projectPath (`--project`) | "" | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: chooses what to open; the settings layers depend on it |
| HostConfig.cpp:91, HostConfig.hpp:26 | sceneOverride (`--scene`) | "" | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: the manifest's bootScene is the persisted home |
| HostConfig.cpp:92, HostConfig.hpp:39 | screenshotPath (`--screenshot`) | "" | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: verification output |
| HostConfig.cpp:93, HostConfig.hpp:44 | printEngineInfo | false | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: the Hub's ABI probe |
| HostConfig.cpp:94, HostConfig.hpp:67 | headless | false | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: a run mode; it requires `--frames` |
| HostConfig.cpp:96, HostConfig.hpp:73 | fixedDtSeconds (`--fixed-dt`) | 1/60 (s); also the string "0.0166666666666666666" | DERIVED | — (from sim.fixedHz) | — | — | — | — | — | Y | FLAG ONLY (headless); its default should read 1/sim.fixedHz instead of repeating 1/60 |
| HostConfig.cpp:98, HostConfig.hpp:95 | fixedTimeSeconds | unset | CONSTANT | — | — | — | — | — | — | Y | FLAG ONLY: pins the clock for verification |
| HostConfig.cpp:101-103, HostConfig.hpp:99,103,206 | probes / cvarSets / reportPath | empty | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: `--set` is the command-line way into settings, not a setting itself |
| HostConfig.cpp:104-119, HostConfig.hpp:223-292 | dump-layout, play-as, view-mode, select-name, tool, open-asset, select-asset, select-in-document | "" | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: one-shot editor boot inputs (view-mode only seeds over the persisted editor.viewport state) |
| HostConfig.cpp:120, HostConfig.hpp:305-306 | windowWidth / windowHeight (`--window-size`) | 0x0 (means 1280x720) | SETTING | render.window.width / height (see the window section) | RenderWindowSettings | PlayerSafe | Project | Restart | 64..8192 | N | BOTH: the flag stays automation-only; ordinary window size becomes the cvar |
| HostConfig.hpp:303-304 | kMinWindowSide / kMaxWindowSide | 64 / 8192 (px) | CONSTANT | — | — | — | — | — | — | N | Flag validation bounds; they become the range metadata of render.window.*, not settings |
| HostConfig.cpp:123, HostConfig.hpp:158 | settleAttempts | 0 | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: verification harness |
| HostConfig.cpp:129, HostConfig.hpp:169 | settleTimeoutMs | 5000 (ms) | CONSTANT | — | — | — | — | — | — | N | Test-only: default of a verification flag inherited from Playwright; already set per run |
| HostConfig.cpp:136-145, HostConfig.hpp:184-203 | compare / bless / maxDiffPixels / maxDiffPixelRatio | "", false, unset | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: golden comparison |
| HostConfig.cpp:170 | `--nri-graph` | no-op | CONSTANT | — | — | — | — | — | — | N | Deprecated flag kept on purpose as a no-op; nothing to configure |
| HostConfig.cpp:174, HostConfig.hpp:334 | crashGpuFrame | 0 | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: dev fault trigger (compiled out of Dist) |
| HostConfig.cpp:176, HostConfig.hpp:342 | hangMainFrame | 0 | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: dev hang trigger |
| HostConfig.cpp:180, HostConfig.hpp:365-367 | pickProbe / X / Y | false / 0 / 0 | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: dev desk check |
| HostConfig.hpp:387 | kHangMainSeconds | 15 (s) | DERIVED | (diagnostics.hangSeconds + 3) | — | — | — | — | — | N | Defined as Core's hangSeconds (12) plus 3; should be computed from it |
| ArcaneClient/src/Arcane/Host/FramePerf.hpp:34 | `--perf` report interval | 60 (frames) | SETTING | diagnostics.perfLogIntervalFrames | DiagnosticsSettings | Game | Pref-P | Live | 1..10000 | N | Averaging window; the help text at HostConfig.cpp:88 repeats "60"; user 2026-10-06: --perf stays usable in Dist |

### Host boot, window and splash
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Platform/Window.hpp:19-20 | WindowDesc width / height | 1280 x 720 (px) | SETTING | render.window.width / render.window.height | RenderWindowSettings | PlayerSafe | Project | Restart | 64..8192 | N | Player-facing; every golden is captured at this size, so the default must not change |
| Window.hpp:21 | resizable | true | SETTING | render.window.resizable | RenderWindowSettings | Game | Project | Restart | bool | N | A per-project choice |
| Window.hpp:18 | title | "Arcane" | CONSTANT | — | — | — | — | — | — | N | Struct default for tests and tools; hosts override it |
| ArcaneClient/src/Arcane/Host/GpuContext.cpp:25 | host window title | "Arcane Runtime" | DERIVED | (project manifest name) | — | — | — | — | — | N | Every shipped game gets this title; should come from the project, as ProductNameFor does (ArcaneRuntime/src/main.cpp:36) |
| GpuContext.cpp:66 | forced Vulkan to D3D12 fallback when a foreign module is injected | on | SETTING | render.vulkan.foreignModuleFallback | RenderDeviceSettings | Game Dev | Pref-P | Restart | bool | N | A developer may want to force Vulkan anyway to reproduce the crash |
| ArcaneClient/src/Arcane/Host/BootSplashWindow.cpp:373-374 | splash width / height | 480 x 270 (px) | SETTING | app.splash.width / app.splash.height | AppSplashSettings | Game | Project | Restart | 64..4096 | N | Per-project branding (reconciled R1) |
| BootSplashWindow.cpp:390 | backgroundRgb | 0x0D0D0F | SETTING | app.splash.backgroundColor | AppSplashSettings | Game | Project | Restart | rgb | N | Branding colour (reconciled R1: the same setting as ProjectManifest.hpp:40; (0.05,0.05,0.06) and 0x0D0D0F are one colour, one default 0x0D0D0F) |
| BootSplashWindow.cpp:332 | status text colour | RGB(160,160,160) | SETTING | app.splash.textColor | AppSplashSettings | Game | Project | Restart | rgb | N | Branding colour (reconciled R1) |
| BootSplashWindow.cpp:50, :285, :327-328 | kTextRowHeightPx / kMarginPx / text inset | 24 / 12 / 12 (px) | DERIVED | — | — | — | — | — | — | N | Splash layout metrics; base px x 1 with an ARC_CONSTANT base (s16.11 applied to the splash, which has no UI scale; was app.splash.textRowPx / marginPx) (reconciled R1) |
| BootSplashWindow.cpp:42-43 | kUserSetProgress / kUserLoadImage | 1 / 2 | CONSTANT | — | — | — | — | — | — | N | Window-message IDs (in-process protocol) |
| ArcaneRuntime/src/main.cpp:212 | splash image | "data/images/arcane_logo.png" | SETTING | app.splash.image | AppSplashSettings | Game | Project | Restart | asset path | N | Every game boots on the engine logo (reconciled R1) |
| ArcaneClient/src/Arcane/Host/ProjectBoot.cpp:168,183,184,215,224,255,256,283,284,292,509,510,537,596 | BootStage weights | 5,1,1,25,45,3,2,2,9,1,5,3,1,2 | CONSTANT | — (allow-list candidate) | — | — | — | — | — | N | capacity hint, no observable preference; ARC_CONSTANT marker (reconciled). Progress-bar share estimates; low value as cvars |
| ProjectBoot.cpp:134 | kStride (scan progress throttle) | 32 (files) | SETTING | boot.scanProgressStride | BootSettings | Game Dev | Pref-P | Live | 1..4096 | N | Status-line throttle; hand-copied in EditorAppProject.cpp (lockstep) |
| ArcaneClient/src/Arcane/Host/BootSequence.cpp:279 | splash pump wait | 8 (ms) | SETTING | boot.splashPumpMs | BootSettings | Game Dev | Pref-P | Restart | 1..100 | N | Repaint cadence during boot |
| ArcaneClient/src/Arcane/Host/ProjectBoot.hpp:120 | SetBaseContext("demo") | "demo" (string) | SETTING | input.baseContext | InputSettings | Game | Project | NextWorld | context name | N | Every project's base input context is named "demo" |
| ProjectBoot.hpp:207 | BootContext::cvarPermission | Permission::Player | CONSTANT | — | — | — | — | — | — | N | Security: default-deny for the runtime's settings permission |

### Runtime host (ArcaneRuntime)
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneRuntime/src/RuntimeFrame.cpp:288 | wall-clock simDt clamp | 0.25 (s) | SETTING | sim.maxFrameDeltaSeconds | SimSettings | Game | Project | Live | 0.01..1.0 | Y | Spiral-of-death guard; changes how many fixed steps run after a hitch (`--fixed-dt` runs bypass it) |
| RuntimeFrame.cpp:182 | sleep while minimized | 1 (ms) | SETTING | app.window.minimizedSleepMs | AppWindowSettings | Game Dev | Pref-P | Live | 0..100 | N | Background throttle (UE has a similar idle option) (reconciled R1) |
| RuntimeFrame.cpp:756 | back-off after a skipped acquire | 1 (ms) | CONSTANT | — | — | — | — | — | — | N | OS scheduler floor; anything smaller is a busy spin |
| RuntimeFrame.cpp:302-310 (called at RuntimeApp.cpp:810) | "ArcaneRuntime" debug HUD | always drawn | SETTING | runtime.hud.show | RuntimeSettings | Game | Pref-P | Live | bool | N | Shown in every build including Dist; the default must stay on (goldens include it). (S6-45 amendment) Game, not Game Dev: ruling R4 lets a Dist player turn the HUD on (S6-25) |
| RuntimeFrame.cpp:354 | console window first-use width | 640 (px) | SETTING | console.windowWidth | ConsoleSettings | Game Dev | Pref-P | Live | 64..8192 | N | Only used on first open (ImGui ini takes over after) (S6-45 amendment): split from console.windowSize (S6-24), one name per value |
| RuntimeFrame.cpp:354 | console window first-use height | 280 (px) | SETTING | console.windowHeight | ConsoleSettings | Game Dev | Pref-P | Live | 64..8192 | N | as console.windowWidth (S6-45 amendment) |
| RuntimeFrame.cpp:252,256-258 | action names console_toggle / quit / reload_plugin / reload_plugin_fresh | strings | CONSTANT | — | — | — | — | — | — | N | Action-name contract with EngineConfig/input.json; the key bindings are the setting |
| ArcaneRuntime/src/RuntimeApp.cpp:311 | shader compile debounce | 0.2 (s) | SETTING | render.shader.compileDebounceSeconds | RenderShaderSettings | Game Dev | Pref-P | Restart | 0..2 | N | Hot-reload latency; also sets the 12-frame settle boundary in goldens (reconciled R1) |
| RuntimeApp.cpp:103 | enableAudioDevice = (maxFrames == 0) | derived | DERIVED | — | — | — | — | — | — | N | Comes from `--frames` |
| RuntimeApp.cpp:532 | offscreenNodes.hostHud | true | CONSTANT | — | — | — | — | — | — | N | Host topology: a host context always presents its chrome |
| RuntimeApp.cpp:1568, RuntimeFrame.cpp:360 | cvar permission for the runtime console | Permission::Player | CONSTANT | — | — | — | — | — | — | N | Security: raising it would let players set non-player-safe values |
| ArcaneRuntime/src/main.cpp:24 | D3D12SDKVersion | 619 | CONSTANT | — | — | — | — | — | — | N | Must match the vendored Agility redistributable |
| main.cpp:25 | D3D12SDKPath | ".\\D3D12\\" | CONSTANT | — | — | — | — | — | — | N | Install layout |
| main.cpp:87,90 | diag.unattended / launchMonitor | = headless / !headless | DERIVED | — | — | — | — | — | — | N | Comes from `--headless` |
| main.cpp:36 | ProductNameFor fallback | "Arcane Runtime" | DERIVED | — | — | — | — | — | — | N | Derived from the project path |

### Renderer device creation (D3D12, Vulkan, NRI)
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Render/RenderDeviceDesc.hpp:19 | backend | D3D12 | DERIVED | (render.backend) | — | — | — | — | — | N | Filled from render.backend |
| RenderDeviceDesc.hpp:21,23 | enableValidation | Debug true, otherwise false | SETTING | render.debug.validation | RenderDebugSettings | Game Dev | Pref-P | Restart | bool | N | Today a Release build cannot turn validation on without recompiling |
| RenderDeviceDesc.hpp:31 | enableD3D12DebugLayer | false | SETTING | render.debug.d3d12DebugLayer | RenderDebugSettings | Game Dev | Pref-P | Restart | bool | N | Opt-in; breaks on desks with injected window hooks |
| RenderDeviceDesc.hpp:54 | enableSyncValidation | false | SETTING | render.debug.vkSyncValidation | RenderDebugSettings | Game Dev | Pref-P | Restart | bool | N | Expensive opt-in |
| ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp:176-178, ArcaneClient/src/Arcane/Host/OffscreenVehicle.cpp:48-50 | Debug forces all three on | true | DERIVED | — | — | — | — | — | — | N | These become the per-configuration defaults of the three cvars |
| ArcaneClient/src/Arcane/Render/Nri/NriDevice.cpp:261,319 | enableNRIValidation | = enableValidation | DERIVED | — | — | — | — | — | — | N | Follows render.debug.validation |
| NriDevice.cpp:266,320 | enableMemoryZeroInitialization | false | CONSTANT | — | — | — | — | — | — | N | Must stay false while VK_EXT_zero_initialize_device_memory is absent; turning it on is a bug |
| NriDevice.cpp:235,239,303,306 | queueNum / queueFamilyNum | 1 | CONSTANT | — | — | — | — | — | — | N | Must equal the queues created; NRI would fetch queues that do not exist |
| NriDevice.cpp:316 | d3dShaderExtRegister | 0 (NRI default) | CONSTANT | — | — | — | — | — | — | N | Shader contract for the vendor-extension register (NVAPI/AGS are not vendored) |
| NriDevice.cpp:317 | d3dZeroBufferSize | 0 (= NRI's 4 MB) | SETTING | render.d3d12.zeroBufferBytes | RenderD3d12Settings | Game Dev | Project | Restart | 0..64 MiB | N | An NRI internal budget |
| NriDevice.cpp:326 | disableD3D12EnhancedBarriers | false | SETTING | render.d3d12.enhancedBarriers | RenderD3d12Settings | Game Dev | Pref-P | Restart | bool | N | Debugging fallback for driver bugs |
| NriDevice.cpp:331 | disableNVAPIInitialization | false | CONSTANT | — | — | — | — | — | — | N | NVAPI is not vendored, so this flag reaches no code |
| NriDevice.cpp:72-86 | kVulkanBindingOffsets | 0 / 128 / 256 / 384 | CONSTANT | — | — | — | — | — | — | N | Shader contract (static_asserted against ShaderConventions) |
| ArcaneClient/src/Arcane/Render/DeviceCreationD3D12.cpp:401-402 | adapter choice | index 0, HIGH_PERFORMANCE | SETTING | render.adapter | RenderSettings | PlayerSafe | Pref-P | Restart | auto \| index | N | Multi-GPU laptops |
| ArcaneClient/src/Arcane/Render/DeviceCreationVulkan.cpp:391-399 | physical device choice | first discrete, else [0] | SETTING | render.adapter (same cvar) | RenderSettings | PlayerSafe | Pref-P | Restart | auto \| index | N | The Vulkan half of the same choice |
| DeviceCreationD3D12.cpp:443 | D3D_FEATURE_LEVEL_12_0 | 12_0 | CONSTANT | — | — | — | — | — | — | N | Minimum hardware |
| DeviceCreationVulkan.cpp:45-46 | kApiVersion | VK 1.3 | CONSTANT | — | — | — | — | — | — | N | Minimum API (Vulkan 1.3 features are chained) |
| DeviceCreationD3D12.cpp:428 | EnableD3D12Dred() | always on | CONSTANT | — | — | — | — | — | — | N | Crash-evidence policy: diagnostics must not be configurable off (ProjectBoot.hpp:128-132) |
| DeviceCreationD3D12.cpp:171-173, :487-489 | SetBreakOnSeverity | FALSE for corruption, error and warning | SETTING | render.debug.breakOnSeverity | RenderDebugSettings | Game Dev | Pref-P | Restart | none \| corruption \| error \| warning | N | Debugger workflow |
| DeviceCreationD3D12.cpp:499-503 | info-queue deny list | INFO, MESSAGE | SETTING | render.debug.minSeverity | RenderDebugSettings | Game Dev | Pref-P | Restart | info..error | N | Validation verbosity |
| DeviceCreationVulkan.cpp:367 | messenger severity | error \| warning | SETTING | render.debug.minSeverity (same cvar) | RenderDebugSettings | Game Dev | Pref-P | Restart | info..error | N | Vulkan half of the same verbosity |
| DeviceCreationVulkan.cpp:100 | kValidationLayer | "VK_LAYER_KHRONOS_validation" | CONSTANT | — | — | — | — | — | — | N | Fixed name in the Vulkan API |
| DeviceCreationVulkan.cpp:576 | queue priority | 1.0 | CONSTANT | — | — | — | — | — | — | N | Only one queue, so the priority means nothing |
| DeviceCreationD3D12.cpp:728 | kDeviceArmorRefs | 65536 (refs) | SETTING | render.d3d12.deviceArmorRefs | RenderD3d12Settings | Game Dev | Pref-P | Restart | 0..2^24 | N | Size of the workaround for foreign over-release |

### Swapchain and frame pacing
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Render/FramePacing.hpp:16 | kSwapchainFramesInFlight | 2 (frames) | SETTING | render.framesInFlight | RenderSettings | Game | Project | Restart | 2..3 | N | Latency against throughput; needs a kMaxFramesInFlight storage constant (see Notes) |
| ArcaneClient/src/Arcane/Render/Nri/NriSwapChain.cpp:132 | textureNum | framesInFlight + 1 | DERIVED | — | — | — | — | — | — | N | NRI's recommended shape |
| NriSwapChain.cpp:150 | queuedFrameNum | = framesInFlight | DERIVED | — | — | — | — | — | — | N | Kept aligned with the fence pacing |
| NriSwapChain.cpp:138 | SwapChainFormat | BT709_G22_8BIT | CONSTANT | — | — | — | — | — | — | N | Tonemap output contract (SDR, display-referred); HDR output is future work |
| NriSwapChain.cpp:145 | ALLOW_TEARING never set | off | SETTING | render.allowTearing | RenderSettings | PlayerSafe | Project | Restart | bool | N | Only matters with vsync off (variable-refresh displays) |
| NriSwapChain.hpp:220, NriGraphContext.hpp:1573 | m_vsync | true | DERIVED | (render.vsync) | — | — | — | — | — | N | Follows render.vsync |
| NriSwapChain.cpp:39, NriGraphContext.cpp:97, ArcaneClient/src/Arcane/Render/GpuInstrumentation.cpp:38 | fence poll sleep | 1 (ms) | CONSTANT | — | — | — | — | — | — | N | OS timer floor; smaller is a spin |
| NriSwapChain.cpp:40, NriGraphContext.cpp:98, GpuInstrumentation.cpp:47 | fence poll window | 15 (s) | DERIVED | (diagnostics.gpuStallSeconds + margin) | — | — | — | — | — | N | Hidden coupling to Core's gpuStallSeconds (8 s) |

### Render graph context and budgets
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| NriGraphContext.cpp:60 | kUploadRingBytesPerFrame | 4 MiB | SETTING | render.uploadRingBytesPerFrame | RenderSettings | Game Dev | Project | Restart | 1..256 MiB | N | Per-frame upload budget; it already counts overflows |
| NriGraphContext.hpp:276 | kGraphCanvasFormat | RGBA16_SFLOAT | SETTING | render.canvasFormat | RenderSettings | Game Dev | Project | Restart | rgba16f \| r11g11b10f | N | Precision against bandwidth; affects goldens |
| NriGraphContext.hpp:286 | kGraphDepthFormat | D32_SFLOAT | SETTING | render.depthFormat | RenderSettings | Game Dev | Project | Restart | d32 \| d24s8 | N | Depth precision |
| NriGraphContext.hpp:297 | kGraphOffscreenFormat | BGRA8_UNORM | CONSTANT | — | — | — | — | — | — | N | Capture, readback and PNG contract, plus byte-equal references |
| NriGraphContext.cpp:66 | kShaderDir | "data/shaders" | CONSTANT | — | — | — | — | — | — | N | Install layout |
| NriGraphContext.cpp:256 | chromeNodes.hostHud | true | CONSTANT | — | — | — | — | — | — | N | Host topology |
| ArcaneClient/src/Arcane/Render/Nri/RenderGraph.hpp:900-903 | kIndexBits / kGenerationBits | 24 / 8 | CONSTANT | — | — | — | — | — | — | N | Handle encoding |
| RenderGraph.hpp:77,460,461,904,909; RenderGraph.cpp:401 | kInvalid / kRgNo* / kNoSlot | sentinels | CONSTANT | — | — | — | — | — | — | N | Sentinels |
| ArcaneClient/src/Arcane/Render/Nri/NriPipelineCache.hpp:102 | kMaxColorAttachments | 4 | CONSTANT | — | — | — | — | — | — | N | Fixed array size in the exported pipeline key (ABI); the hardware maximum is 8 |
| NriPipelineCache.hpp:107 | kInvalidLayout | 0xFFFFFFFF | CONSTANT | — | — | — | — | — | — | N | Sentinel |
| Batch2DNode.cpp:265-273; FullscreenNodes.cpp:299-304,1177-1180; GridNode.cpp:111-113; MeshCullNode.cpp:96; MeshNode.cpp:305-309; PickOutlineNodes.cpp:797-800 (all under ArcaneClient/src/Arcane/Render/Nri/nodes/) | descriptor-pool sizes | computed | DERIVED | — | — | — | — | — | — | N | Products of framesInFlight and the per-node caps below |

### Batch2D (sprites)
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp:45 | kCanvasClear | {0.02, 0.02, 0.04, 1} (linear RGBA) | SETTING | render.clearColor | RenderSettings | Game | Project | Live | rgba | N | Background colour; affects every golden, so the default must stay |
| Batch2DNode.cpp:284-286 | sprite sampler filter | LINEAR | SETTING | render.sprite.filter | RenderSpriteSettings | Game | Project | Restart | linear \| point | N | Pixel-art projects need point filtering |
| Batch2DNode.cpp:287-288 | sprite address mode | CLAMP_TO_EDGE | CONSTANT | — | — | — | — | — | — | N | Stops atlas edge bleed; changing it is a bug |
| Batch2DNode.cpp:289; FullscreenNodes.cpp:282,1134; MeshNode.cpp:392; ArcaneClient/src/Arcane/ImGui/ImGuiNri.cpp:224 | mipMax | 16.0 | CONSTANT | — | — | — | — | — | — | N | Means "all mips" for any size up to 64K; no clamp |
| Batch2DNode.hpp:249 | kMaxMaterialSlots | 8 | SETTING | render.batch2d.maxMaterialSlots | RenderBatch2dSettings | Game Dev | Project | Restart | 1..64 | N | Pool cap; above it materials fall back with one ERROR |
| Batch2DNode.hpp:250 | kMaxMaterialTextures | 8 | SETTING | render.batch2d.maxMaterialTextures | RenderBatch2dSettings | Game Dev | Project | Restart | 1..16 | N | Pool cap (check the material-template codegen ceiling) |
| Batch2DNode.hpp:271 | kMaxSpriteTextures | 64 | SETTING | render.batch2d.maxSpriteTextures | RenderBatch2dSettings | Game | Project | Restart | 8..512 | N | Overflow is silent and still exits 0 (header comment); content-dependent |
| Batch2DNode.hpp:276 | kMaterialCbMaxBytes | 256 (bytes) | SETTING | render.batch2d.materialCbBytes | RenderBatch2dSettings | Game Dev | Project | Restart | multiple of 256 | N | Ceiling on material parameter size |
| Batch2DNode.hpp:279; Batch2DNode.cpp:261-262 | kCbRegionsPerFrame / kBuiltInSets / kMaterialSets | computed | DERIVED | — | — | — | — | — | — | N | Computed from the caps |
| Batch2DNode.cpp:85,103-104,93-94; Batch2DNode.hpp:155,443,450; Batch2DNode.cpp:217 | vertex alignment 16, FNV constants, shader names, kNoRange, kBuiltInCount, kShaderPairBase, white texel | — | CONSTANT | — | — | — | — | — | — | N | Alignment, math, shader names, IDs, identity texel |
| ArcaneClient/src/Arcane/Render/Batcher2D.cpp:24; Batcher2D.hpp:207-210; Batcher2D.cpp:426-428 | built-in material IDs; sort-key bit layout | 0..2, 0xFFFF; shifts 48/32/16 | CONSTANT | — | — | — | — | — | — | N | ID scheme and key encoding |

### Mesh pass, cull, GPU scene, visibility
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp:297-299 | MeshSceneDesc default light (direction, colour, ambient) | (0,0,1), (1,1,1), (0.05,0.05,0.05) | SETTING | render.mesh.defaultLight.direction / .color / .ambient | RenderMeshDefaultLightSettings | Game | Project | Live | dir vec3; colour 0..16 | N | The only scene light until a light component exists (RuntimeFrame.cpp:658-664); affects runtime mesh goldens |
| ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp:298 | default light intensity | 1.0 | SETTING | render.mesh.defaultLight.intensity | RenderMeshDefaultLightSettings | Game | Project | Live | 0..16 | N | (S6-45 amendment): the colour x intensity split (S6-19, controller ruling) |
| ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.hpp:299 | default ambient intensity | 1.0 | SETTING | render.mesh.defaultLight.ambientIntensity | RenderMeshDefaultLightSettings | Game | Project | Live | 0..16 | N | as render.mesh.defaultLight.intensity (S6-45 amendment) |
| MeshNode.cpp:101-103 | frame CB light defaults | (0,0,1), 1, 0 | DERIVED | — | — | — | — | — | — | N | Overwritten every frame from MeshSceneDesc |
| MeshNode.cpp:391 | root sampler anisotropy | 16 | SETTING | render.textureAnisotropy | RenderSettings | PlayerSafe | Project | Restart | 1..16 (16 is the hardware maximum) | N | Standard quality option; baked into the pipeline layout |
| MeshNode.cpp:389-390 | mesh address mode | REPEAT | CONSTANT | — | — | — | — | — | — | N | Mesh-material UV tiling contract (belongs to the material, not a global) |
| MeshNode.cpp:79 | kDepthClear | 1.0 | CONSTANT | — | — | — | — | — | — | N | Standard-Z depth with a LESS compare; changing it is a bug |
| MeshNode.hpp:565 | kBindlessCapacity | 256 | CONSTANT | — | — | — | — | — | — | N | Must equal kMeshBindlessCapacity in data/shaders/mesh.hlsl:155 |
| MeshNode.hpp:645 | kInitialResidentSlots | 16 | CONSTANT | — (allow-list candidate) | — | — | — | — | — | N | capacity hint, no observable preference; ARC_CONSTANT marker (reconciled). Reserve hint only |
| MeshNode.hpp:547; GridNode.hpp:175 | kFrameCbMaxBytes | 256 | CONSTANT | — | — | — | — | — | — | N | Constant-buffer placement alignment; the struct must fit |
| MeshNode.hpp:346, :216 | kMeshRootDirect; default baseColor white | 1; (1,1,1,1) | CONSTANT | — | — | — | — | — | — | N | Root layout index; identity colour |
| MeshCullNode.hpp:26 (cvar at MeshCullNode.cpp:21) | kMeshCullEnabled / render.meshCull | true | SETTING (already a cvar) | render.meshCull | RenderSettings | Game Dev | Project | Live | bool | N | Exists; only needs audience, scope and apply metadata |
| MeshCullNode.hpp:29 | kMeshCullThreads | 64 | CONSTANT | — | — | — | — | — | — | N | Must match numthreads(64) in data/shaders/mesh_cull.hlsl:60 |
| ArcaneClient/src/Arcane/Render/Nri/GpuScene.hpp:63 | kInitialRows | 256 (rows) | SETTING | render.gpuScene.initialRows | RenderGpuSceneSettings | Game Dev | Project | Restart | 16..65536 | N | Initial capacity (grows) |
| GpuScene.hpp:64 | kScratchRows | 64 (rows per frame) | SETTING | render.gpuScene.scratchRowsPerFrame | RenderGpuSceneSettings | Game Dev | Project | Restart | 16..4096 | N | Hard cap; ad-hoc instances past it are dropped with a WARN |
| GpuScene.cpp:31,36 | kRingAlign / kVisibilityRegionAlign | 16 / 256 | CONSTANT | — | — | — | — | — | — | N | Hardware alignment |
| ArcaneClient/src/Arcane/Render/GpuSceneTypes.hpp:30-38 | instance flag bits, invalid material slot | bit layout | CONSTANT | — | — | — | — | — | — | N | Shader contract (gpu_scene.hlsli) |
| ArcaneClient/src/Arcane/Render/GpuSceneSync.hpp:73; GpuSceneTypes.hpp:124 | alphaCutoff default | 0.5 | CONSTANT | — | — | — | — | — | — | N | glTF 2.0's default alphaCutoff (format) |
| ArcaneClient/src/Arcane/Render/VisibilitySystem.hpp:39 | kVisibilitySlack | 0.25 (m) | SETTING | render.cull.frustumSlackMeters | RenderCullSettings | Game Dev | Project | Live | 0..10 | N | Conservativeness of CPU coarse culling |
| ArcaneClient/src/Arcane/Render/Nri/MeshResidencyBudget.hpp:61 (default argument at NriMeshBufferCache.hpp:139) | kMeshResidencyBudgetBytes | 512 MiB | SETTING | render.mesh.residencyBudgetBytes | RenderMeshSettings | Game | Project | Restart | 64 MiB..16 GiB | N | Memory budget; eviction threshold |

### Post chain, tonemap and grid
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Render/Nri/nodes/FullscreenNodes.hpp:254 | PostChainNode::kMaxPasses | 8 | SETTING | render.post.maxPasses | RenderPostSettings | Game Dev | Project | Restart | 1..32 | N | Pool cap; longer chains fall back with an ERROR |
| FullscreenNodes.hpp:255 | kMaxTextures | 8 | SETTING | render.post.maxTextures | RenderPostSettings | Game Dev | Project | Restart | 1..16 | N | Pool cap |
| FullscreenNodes.hpp:258 | kMaxInputs | 4 | CONSTANT | — | — | — | — | — | — | N | static_assert equal to kMaxPassInputs (FullscreenNodes.cpp:158), the post asset format's ceiling |
| FullscreenNodes.hpp:259 | kCbMaxBytes | 256 | SETTING | render.post.materialCbBytes | RenderPostSettings | Game Dev | Project | Restart | multiple of 256 | N | Ceiling on material parameter size |
| FullscreenNodes.hpp:262-264,136,356,475; FullscreenNodes.cpp:43-44 | region indices, sentinels, pair IDs, FNV | — | CONSTANT | — | — | — | — | — | — | N | Layout, IDs, math |
| ArcaneClient/src/Arcane/Render/Nri/nodes/GridNode.hpp:106 | minorSpacing | 1.0 (m) | SETTING | editor.viewport.grid3D.minorSpacing | EditorGrid3DSettings | Editor | Pref-M | Live | 0.01..100 | N | Grid preference (reconciled R1) |
| GridNode.hpp:107 | majorEvery | 10.0 (m) | SETTING | editor.viewport.grid.majorEvery | EditorGridSettings | Editor | Pref-M | Live | 1..1000 | N | Grid preference (reconciled R1: one grid family; the 2D grid's decade base stays CONSTANT) |
| GridNode.hpp:108 | fadeDistance | 200 (m) | SETTING | editor.viewport.grid3D.fadeDistance | EditorGrid3DSettings | Editor | Pref-M | Live | 1..10000 | N | Grid preference (reconciled R1) |
| GridNode.hpp:112 | minorColor | (0.5,0.5,0.5,0.35) | SETTING | editor.viewport.grid.lineColor / editor.viewport.grid.minorAlpha | EditorGridSettings | Editor | Pref-M | Live | rgb / 0..1 | N | minorColor = lineColor (0.5,0.5,0.5) at minorAlpha 0.35; one family with ViewportGrid; affects editor goldens (reconciled R1) |
| GridNode.hpp:113 | majorColor | (0.6,0.6,0.6,0.6) | SETTING | editor.viewport.grid3D.majorColor | EditorGrid3DSettings | Editor | Pref-M | Live | rgba | N | 3D-only major line colour; affects editor goldens (reconciled R1) |
| GridNode.hpp:124-126 | kAxisX/Y/ZColor | red / green / blue | CONSTANT | — | — | — | — | — | — | N | ARC_CONSTANT("pending axis unification re-bless"): the 3D grid axis colours stay values until a post-sweep task unifies them with the gizmo and inspector axis tokens in one deliberate re-bless (S5-2 review, decision 1 option A; was DERIVED from editor.theme.axis{X,Y,Z}, ruling I4) |
| GridNode.cpp:45 | kMinHalfExtent | 2000 (m) | SETTING | editor.viewport.grid3D.minHalfExtent | EditorGrid3DSettings | Editor Dev | Pref-M | Live | 10..1e6 | N | Grid quad size (reconciled R1) |
| GridNode.cpp:46 | kHalfExtentPerMetre | 100 | SETTING | editor.viewport.grid3D.extentPerAltitude | EditorGrid3DSettings | Editor Dev | Pref-M | Live | 1..1000 | N | How the grid grows with camera altitude (reconciled R1) |
| GridNode.cpp:59 | params CB default {1,10,200,2000} | — | DERIVED | — | — | — | — | — | — | N | Copy of the struct defaults; overwritten every frame |
| GridNode.cpp:37-38; GridNode.hpp:220 | shader names / pair ID | — | CONSTANT | — | — | — | — | — | — | N | IDs |

### Pick and selection outline
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp:100 | kSelectColor | (1, 0.65, 0.10, 1) | DERIVED | (editor.theme.amber) | — | — | — | — | — | N | Theme colour; affects outline goldens (reconciled R1: (1,0.65,0.10) = kAmber exactly) |
| PickOutlineNodes.cpp:101 | kHoverColor | (0.25, 0.70, 1, 1) | DERIVED | (editor.theme.graph.hoverBorder) | — | — | — | — | — | N | Theme colour (reconciled R1: (0.25,0.70,1) = kGraphNodeHovBorderColor exactly) |
| PickOutlineNodes.cpp:102 | kSelectThickPx | 3 (px) | SETTING | render.outline.selectWidthPx | RenderOutlineSettings | Game Dev | Pref-P | Live | 1..kOutlineMaxThicknessPx | N | The spec names outline width explicitly (reconciled R1: ArcaneClient's OutlineNode cannot declare an Editor-audience cvar) |
| PickOutlineNodes.cpp:103 | kHoverThickPx | 3 (px) | SETTING | render.outline.hoverWidthPx | RenderOutlineSettings | Game Dev | Pref-P | Live | 1..32 | N | Preference (reconciled R1: ArcaneClient's OutlineNode cannot declare an Editor-audience cvar) |
| PickOutlineNodes.cpp:104 | kEdgeSoftPx | 1 (px) | SETTING | render.outline.edgeSoftnessPx | RenderOutlineSettings | Game Dev | Pref-P | Live | 0..4 | N | Anti-aliasing ramp width (reconciled R1: ArcaneClient's OutlineNode cannot declare an Editor-audience cvar) |
| PickOutlineNodes.hpp:151 | kOutlineMaxThicknessPx | 32 (px) | SETTING | render.outline.maxThicknessPx | RenderOutlineSettings | Game Dev | Pref-P | Restart | 1..256 | N | Drives the jump-flood step count; must stay at or above the width settings (reconciled R1: ArcaneClient's OutlineNode cannot declare an Editor-audience cvar) |
| ArcaneClient/src/Arcane/Render/PickEmit.hpp:177 | kPickSupersample | 2 | SETTING | render.outline.supersample | RenderOutlineSettings | Game Dev | Pref-P | Restart | 1..4 | N | Visible in pixels; changing it re-blesses the outline goldens (reconciled R1: ArcaneClient's OutlineNode cannot declare an Editor-audience cvar) |
| PickOutlineNodes.hpp:485 | kMaxJfaSteps | 16 | CONSTANT | — | — | — | — | — | — | N | Constant-buffer region layout; log2 ceiling of the schedule |
| PickOutlineNodes.hpp:489; PickOutlineNodes.cpp:66 | kMaxSelectedIds / selectedIds[64] | 64 | CONSTANT | — | — | — | — | — | — | N | static_assert to a 256-byte cbuffer array the outline shader reads (PickOutlineNodes.cpp:74) |
| PickOutlineNodes.hpp:492,496-498,334-335,564-566,138,144 | kCbMaxBytes 288, region indices, pair IDs, R32_UINT, RGBA16_SNORM | — | CONSTANT | — | — | — | — | — | — | N | Layout, IDs, shader encoding |
| PickOutlineNodes.cpp:400,1309,428 | depth clear 1.0; vertex alignment 16 | — | CONSTANT | — | — | — | — | — | — | N | Depth convention; alignment |
| NriGraphContext.cpp:557,567,1478 | jfaStepCount | min(OutlineJfaStepCount(32), 16) | DERIVED | — | — | — | — | — | — | N | Computed from render.outline.maxThicknessPx |

### Texture and mesh caches
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Render/Nri/NriTextureCache.hpp:302 | kPendingCookRepollInterval | 32 (resolves) | SETTING | assets.cook.pendingRepollInterval | AssetsCookSettings | Game Dev | Project | Live | 1..1024 | N | Repoll cadence for textures still being cooked |
| NriTextureCache.cpp:386-397 | pending-cook placeholder, checker colour | magenta | SETTING | render.debug.pendingCookChecker | RenderDebugSettings | Game Dev | Pref-P | Restart | rgba | N | Debug convention; must never look like the "refused" white texel (S6-45 amendment): the pair split, one name per value |
| NriTextureCache.cpp:386-397 | pending-cook placeholder, alternate colour | (16,16,16) | SETTING | render.debug.pendingCookCheckerAlt | RenderDebugSettings | Game Dev | Pref-P | Restart | rgba | N | as render.debug.pendingCookChecker (S6-45 amendment) (S6-16) |
| NriTextureCache.hpp:278 | BC block-row math | (w+3)/4*16 | CONSTANT | — | — | — | — | — | — | N | BC format math |

### Shader compiler
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Render/ShaderCompiler.cpp:405; ShaderCompiler.hpp:111 | debounce | 0.2 (s) | DERIVED | (render.shader.compileDebounceSeconds) | — | — | — | — | — | N | Third copy of the value set at RuntimeApp.cpp:311 (reconciled R1) |
| ShaderCompiler.cpp:420 | compile worker count | 1 thread | SETTING | jobs.shaderCompileThreads | JobsSettings | Game Dev | Pref-P | Restart | 1..hardware threads | N | Compile throughput (reconciled R1: JobsSettings) |
| ShaderCompiler.cpp:270-288 | DXC arguments: no -O or -Zi | DXC default (-O3, no debug info) | SETTING | render.shader.debugInfo / render.shader.optimization | RenderShaderSettings | Game Dev | Pref-P | Restart | bool / O0..O3 | N | Source-level shader debugging (PIX/RenderDoc) is impossible today; part of the cache key |
| ArcaneClient/src/Arcane/Render/ShaderConventions.hpp:36-38; ShaderCompiler.hpp:59 | profiles vs/ps/cs_6_5 | SM 6.5 | CONSTANT | — | — | — | — | — | — | N | Minimum shader model contract |
| ShaderConventions.hpp:33-35, :44 | entry names; kSpirvArgs | — | CONSTANT | — | — | — | — | — | — | N | Shader contract (SPIR-V shifts match kVulkanBindingOffsets) |
| ShaderCompiler.cpp:204-205 | FNV | — | CONSTANT | — | — | — | — | — | — | N | Math |
| ShaderCompiler.cpp:244 | read buffer | 64 KiB | CONSTANT | — (allow-list candidate) | — | — | — | — | — | N | capacity hint, no observable preference; ARC_CONSTANT marker (reconciled). I/O buffer size hint |

### ImGui NRI backend and console
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/ImGui/ImGuiNri.hpp:325 | kFirstPoolSets | 64 | SETTING | render.imgui.firstPoolSets | RenderImguiSettings | Game Dev | Project | Restart | 1..1024 | N | Size of the first link in the pool chain |
| ImGuiNri.hpp:326 | kMaxPoolSets | 1024 | SETTING | render.imgui.maxPoolSetsPerLink | RenderImguiSettings | Game Dev | Project | Restart | 64..2048 | N | A choice under the D3D12 2048-sampler heap limit, which is the constant ceiling |
| ImGuiNri.cpp:218-223 | ImGui sampler | LINEAR, clamp | CONSTANT | — | — | — | — | — | — | N | ImGui backend contract (font-atlas sampling) |
| ImGuiNri.cpp:79,50,58; ImGuiNri.hpp:445 | vertex alignment 16, static_asserts, pair ID 0x5000 | — | CONSTANT | — | — | — | — | — | — | N | Layout, IDs |
| ArcaneClient/src/Arcane/ImGui/OffscreenImGuiLayer.hpp:58; OffscreenImGuiLayer.cpp:60 | deltaTime fallback | 1/60 (s) | DERIVED | (sim.fixedHz) | — | — | — | — | — | N | Another copy of 1/60 |
| ArcaneClient/src/Arcane/ImGui/ConsoleInputLine.cpp:38 | input buffer | 512 (chars) | SETTING | console.maxLineChars | ConsoleSettings | Game Dev | Pref-P | Restart | 64..65536 | N | Line-length cap (fixed-size stack buffer) |

### GPU diagnostics (breadcrumbs, crash dump, fault injector)
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Render/GpuBreadcrumbs.hpp:36 | kRingCapacity | 256 (markers) | SETTING | diagnostics.gpu.breadcrumbSlots | DiagnosticsGpuSettings | Game Dev | Project | Restart | 16..4096 | N | Crash-trail depth; sizes the marker buffer |
| ArcaneClient/src/Arcane/Render/GpuCrashReport.hpp:53-54 | kGpuMarkerSlots | = kRingCapacity | DERIVED | — | — | — | — | — | — | N | Follows the ring size |
| GpuCrashReport.hpp:55,61 | values per slot 2; unwritten 0 | — | CONSTANT | — | — | — | — | — | — | N | Marker record layout |
| ArcaneClient/src/Arcane/Render/IGpuCrashBackend.hpp:58-61 | kGpuDumpVersion / tag / header / entry bytes | 1 / 16 / 12 / 32 | CONSTANT | — | — | — | — | — | — | N | Dump file format |
| ArcaneClient/src/Arcane/Render/Nri/NriDiagnostics.cpp:371-379 | fault CB / sink / groups / iterations / out-of-bounds element | 256, 256, 256, 0xFFFFFFFF, 1<<30 | CONSTANT | — | — | — | — | — | — | N | Test-only: values chosen to guarantee a TDR or out-of-bounds fault |
| NriDiagnostics.cpp:384-385,628-631 | fault shader stem / dir; one-off pool | — | CONSTANT | — | — | — | — | — | — | N | Layout; test-only |
| NriDiagnostics.cpp:836 | kRemovalBudget | 45 (s) | SETTING | diagnostics.gpuFault.removalBudgetSeconds | DiagnosticsGpuFaultSettings | Game Dev | Pref-P | Live | 5..300 | N | TdrDelay is configurable per machine |
| NriDiagnostics.cpp:837 | kRemovalPoll | 50 (ms) | SETTING | diagnostics.gpuFault.removalPollMs | DiagnosticsGpuFaultSettings | Game Dev | Pref-P | Live | 1..1000 | N | Poll cadence |
| GpuInstrumentation.cpp:27 | diagnostics.drawMarkers | false | SETTING (already a cvar) | diagnostics.drawMarkers | DiagnosticsSettings | Game Dev | Project | Live | bool | N | Exists; needs metadata |

### Gizmo (Edit)
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Edit/Gizmo.hpp:95 | GizmoSnap::translate | 0.5 (m) | SETTING | editor.gizmo.snap.translate | EditorGizmoSnapSettings | Editor | Pref-P | Live | 0.001..100 | N | The editor sets only `enabled` (ArcaneEditor/src/App/EditorAppFrame.cpp:1303-1304), so nobody can change this today (reconciled R1) |
| Gizmo.hpp:96 | rotationDeg | 15 (deg) | SETTING | editor.gizmo.snap.rotateDegrees | EditorGizmoSnapSettings | Editor | Pref-P | Live | 0.1..90 | N | Same (reconciled R1) |
| Gizmo.hpp:97 | scale | 0.1 (ratio) | SETTING | editor.gizmo.snap.scale | EditorGizmoSnapSettings | Editor | Pref-P | Live | 0.001..10 | N | Same (reconciled R1) |
| Gizmo.cpp:28-39,41-45,285-286,627,726 | handle geometry (kAxisLenPx 70 ... kScreenRingWidthPx 3; bevel 0.25/0.3/0.35; centre 0.42; tip 10) | px (UE-matched) | DERIVED | (editor.gizmo.size) | — | — | — | — | — | N | Base proportions multiplied by gizmoSize; the size is the setting (reconciled R1: gizmoSize lives in the layout ini today, not a cvar; it migrates to editor.gizmo.size) |
| Gizmo.cpp:46 | kHitThreshPx | 8 (px) | SETTING | editor.gizmo.pickRadiusPx | EditorGizmoSettings | Editor | Pref-P | Live | 1..32 | N | Pick tolerance (reconciled R1) |
| Gizmo.cpp:47 | kRingHitSlackPx | 4 (px) | SETTING | editor.gizmo.ringPickSlackPx | EditorGizmoSettings | Editor | Pref-P | Live | 0..32 | N | Pick tolerance (reconciled R1) |
| Gizmo.cpp:48 | kMinQuadAreaPx2 | 4 (px²) | SETTING | editor.gizmo.minPlaneAreaPx2 | EditorGizmoSettings | Editor Dev | Pref-P | Live | 0..100 | N | Hides edge-on plane handles (reconciled R1) |
| Gizmo.cpp:40 | kPlaneEdgeOnCos | 0.2 | SETTING | editor.gizmo.planeEdgeOnCos | EditorGizmoSettings | Editor Dev | Pref-P | Live | 0..1 | N | Same family (reconciled R1) |
| Gizmo.cpp:605 | min drawn length | 2 (px) | SETTING | editor.gizmo.minAxisLenPx | EditorGizmoSettings | Editor Dev | Pref-P | Live | 0..32 | N | Degenerate-axis cutoff (reconciled R1) |
| Gizmo.cpp:49 | kRingSegments | 48 | SETTING | editor.gizmo.ringSegments | EditorGizmoSettings | Editor Dev | Pref-P | Live | 8..256 | N | Tessellation (reconciled R1) |
| Gizmo.cpp:19 | kMinScale | 0.01 | SETTING | editor.gizmo.minScale | EditorGizmoSettings | Editor Dev | Pref-P | Live | 1e-6..1 | N | Floor on scale drags (reconciled R1) |
| Gizmo.cpp:58-60 | kColorX/Y/Z | RGBA set | CONSTANT | — | — | — | — | — | — | N | ARC_CONSTANT("pending axis unification re-bless"): the gizmo axis colours stay values until a post-sweep task unifies them with the grid and inspector axis tokens in one deliberate re-bless (S5-2 review, decision 1 option A; was DERIVED from editor.theme.axis{X,Y,Z}, ruling I4) |
| Gizmo.cpp:61-64 | kColorHot/Screen/ScreenArc/Centre | RGBA set | SETTING | editor.gizmo.color.{hot,screen,screenArc,centre} | EditorGizmoColorSettings | Editor | Pref-P | Live | rgba | N | Theme colours (reconciled R1) |
| Gizmo.cpp:248,253 | Brighten 1.4 / Darken 0.55 | factors | SETTING | editor.gizmo.brighten / editor.gizmo.darken | EditorGizmoSettings | Editor Dev | Pref-P | Live | 0..4 | N | Shading look (reconciled R1) |
| Gizmo.cpp:536,556,581 | hot fill alpha | 0.3 | SETTING | editor.gizmo.hotFillAlpha | EditorGizmoSettings | Editor Dev | Pref-P | Live | 0..1 | N | Look (reconciled R1) |
| Gizmo.cpp:18,20-21 | kEps / kPi / kTau | — | CONSTANT | — | — | — | — | — | — | N | Math |

### Undo (CommandStack)
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Edit/CommandStack.hpp:39-41 | UndoLimits maxSteps / byteBudget / spillThreshold | 100 / 512 MiB / 256 KiB | DERIVED | (editor.undo.*, which already exist) | — | — | — | — | — | N | The editor pushes these in through SetLimits; these are fallback copies of the cvar defaults |
| CommandStack.cpp:111 | spill copy chunk | 1 MiB | CONSTANT | — (allow-list candidate) | — | — | — | — | — | N | capacity hint, no observable preference; ARC_CONSTANT marker (reconciled). I/O chunk hint |
| ArcaneClient/src/Arcane/Edit/ComponentEditCommand.cpp:34 | writer reserve | 256 (bytes) | CONSTANT | — (allow-list candidate) | — | — | — | — | — | N | capacity hint, no observable preference; ARC_CONSTANT marker (reconciled). Reserve hint |

### Physics debug draw
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.hpp:64 | lineThickness | 1.0 (px) | SETTING | debug.physics.lineThickness | PhysicsDebugSettings | Game Dev | Pref-P | Live | 0.5..8 | N | Overlay look |
| PhysicsDebugDraw.hpp:70,77,91,104,111 | drawContacts / drawAabbs / drawVelocities / drawComMarkers / drawOrientations | true / false / true / true / true | SETTING | debug.physics.draw.{contacts,aabbs,velocities,comMarkers,orientations} | DebugPhysicsDrawSettings | Game Dev | Pref-P | Live | bool | N | Overlay toggles (the editor hard-overrides several at EditorAppFrame.cpp:1907) |
| PhysicsDebugDraw.hpp:73 | contactMarkerSize | 0.03 (m) | SETTING | debug.physics.contactMarkerSize | PhysicsDebugSettings | Game Dev | Pref-P | Live | 0.001..1 | N | Look; the comment still cites pixelsPerMeter=100 |
| PhysicsDebugDraw.hpp:94 | velocityScale | 0.15 (s look-ahead) | SETTING | debug.physics.velocityScale | PhysicsDebugSettings | Game Dev | Pref-P | Live | 0..5 | N | Look |
| PhysicsDebugDraw.hpp:100 | velocityRayMinSpeed | 0.05 (m/s) | SETTING | debug.physics.velocityMinSpeed | PhysicsDebugSettings | Game Dev | Pref-P | Live | 0..10 | N | Look |
| PhysicsDebugDraw.hpp:106 | comMarkerSize | 0.05 (m) | SETTING | debug.physics.comMarkerSize | PhysicsDebugSettings | Game Dev | Pref-P | Live | 0.001..1 | N | Look |
| PhysicsDebugDraw.hpp:112 | orientationTickLen | 0.18 (m) | SETTING | debug.physics.orientationTickLen | PhysicsDebugSettings | Game Dev | Pref-P | Live | 0.01..5 | N | Look |
| PhysicsDebugDraw.hpp:216-217 | narrowphase overlay lineThickness / emphasis | 1.5 / 1.0 | SETTING | debug.physics.trace.lineThickness | DebugPhysicsTraceSettings | Game Dev | Pref-P | Live | 0.5..8 | N | Default parameters |
| PhysicsDebugDraw.hpp:217 | narrowphase overlay emphasis | 1.0 | SETTING | debug.physics.trace.emphasis | DebugPhysicsTraceSettings | Game Dev | Pref-P | Live | 0..1 | N | Default parameter (S6-45 amendment): split from the lineThickness row, one name per value (ruling S6-10) |
| PhysicsDebugDraw.cpp:49-53,119-121,127-134,196-197,670 | kCol* (kinematic, static, sensor, contact, AABB, velocity, COM, orient, tree, pair, grids, trace, subject) | RGBA set | SETTING | debug.physics.color.{kinematic,static,sensor,contact,aabb,velocity,com,orient,treeTight,treeFat,treePair,staticGrid,residencyGrid,traceShapeB,traceAxis,traceAxisHi,traceNormal,tracePoint,subject} | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Debug palette |
| PhysicsDebugDraw.cpp:59-68 | kIslandPalette[0] | RGBA | SETTING | debug.physics.color.island0 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (S6-45 amendment): replaces debug.physics.islandPalette (no array CVarType; ruling S6-10) |
| PhysicsDebugDraw.cpp:59-68 | kIslandPalette[1] | RGBA | SETTING | debug.physics.color.island1 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (S6-45 amendment): replaces debug.physics.islandPalette (no array CVarType; ruling S6-10) |
| PhysicsDebugDraw.cpp:59-68 | kIslandPalette[2] | RGBA | SETTING | debug.physics.color.island2 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (S6-45 amendment): replaces debug.physics.islandPalette (no array CVarType; ruling S6-10) |
| PhysicsDebugDraw.cpp:59-68 | kIslandPalette[3] | RGBA | SETTING | debug.physics.color.island3 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (S6-45 amendment): replaces debug.physics.islandPalette (no array CVarType; ruling S6-10) |
| PhysicsDebugDraw.cpp:59-68 | kIslandPalette[4] | RGBA | SETTING | debug.physics.color.island4 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (S6-45 amendment): replaces debug.physics.islandPalette (no array CVarType; ruling S6-10) |
| PhysicsDebugDraw.cpp:59-68 | kIslandPalette[5] | RGBA | SETTING | debug.physics.color.island5 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (S6-45 amendment): replaces debug.physics.islandPalette (no array CVarType; ruling S6-10) |
| PhysicsDebugDraw.cpp:59-68 | kIslandPalette[6] | RGBA | SETTING | debug.physics.color.island6 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (S6-45 amendment): replaces debug.physics.islandPalette (no array CVarType; ruling S6-10) |
| PhysicsDebugDraw.cpp:59-68 | kIslandPalette[7] | RGBA | SETTING | debug.physics.color.island7 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (S6-45 amendment): replaces debug.physics.islandPalette (no array CVarType; ruling S6-10) |
| PhysicsDebugDraw.cpp:149-156 | narrowphase palette, NarrowphaseKind 0 | RGBA | SETTING | debug.physics.color.narrowphase0 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (NarrowphaseKind ordinal 0) (S6-45 amendment): replaces debug.physics.narrowphaseColors (ruling S6-10) |
| PhysicsDebugDraw.cpp:149-156 | narrowphase palette, NarrowphaseKind 1 | RGBA | SETTING | debug.physics.color.narrowphase1 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (NarrowphaseKind ordinal 1) (S6-45 amendment): replaces debug.physics.narrowphaseColors (ruling S6-10) |
| PhysicsDebugDraw.cpp:149-156 | narrowphase palette, NarrowphaseKind 2 | RGBA | SETTING | debug.physics.color.narrowphase2 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (NarrowphaseKind ordinal 2) (S6-45 amendment): replaces debug.physics.narrowphaseColors (ruling S6-10) |
| PhysicsDebugDraw.cpp:149-156 | narrowphase palette, NarrowphaseKind 3 | RGBA | SETTING | debug.physics.color.narrowphase3 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (NarrowphaseKind ordinal 3) (S6-45 amendment): replaces debug.physics.narrowphaseColors (ruling S6-10) |
| PhysicsDebugDraw.cpp:149-156 | narrowphase palette, NarrowphaseKind 4 | RGBA | SETTING | debug.physics.color.narrowphase4 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (NarrowphaseKind ordinal 4) (S6-45 amendment): replaces debug.physics.narrowphaseColors (ruling S6-10) |
| PhysicsDebugDraw.cpp:149-156 | narrowphase palette, NarrowphaseKind 5 | RGBA | SETTING | debug.physics.color.narrowphase5 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (NarrowphaseKind ordinal 5) (S6-45 amendment): replaces debug.physics.narrowphaseColors (ruling S6-10) |
| PhysicsDebugDraw.cpp:149-156 | narrowphase palette, NarrowphaseKind 6 | RGBA | SETTING | debug.physics.color.narrowphase6 | DebugPhysicsColorSettings | Game Dev | Pref-P | Live | rgba | N | Palette entry (NarrowphaseKind ordinal 6) (S6-45 amendment): replaces debug.physics.narrowphaseColors (ruling S6-10) |
| PhysicsDebugDraw.cpp:138 | kManifoldNormalLen | 20 ("world units", so 20 m) | SETTING | debug.physics.manifoldNormalLength | PhysicsDebugSettings | Game Dev | Pref-P | Live | 0.01..100 | N | Look; probably left over from before the switch to metres (see Notes) |
| PhysicsDebugDraw.cpp:139 | kManifoldPointPx | 3 (px) | SETTING | debug.physics.manifoldPointPx | PhysicsDebugSettings | Game Dev | Pref-P | Live | 1..16 | N | Look |
| PhysicsDebugDraw.cpp:747 | kNormalLen | 28 ("world units") | SETTING | debug.physics.trace.normalLength | DebugPhysicsTraceSettings | Game Dev | Pref-P | Live | 0.01..100 | N | Same unit suspicion |
| PhysicsDebugDraw.cpp:183,390,660,672,703,725,739,749,758 | arrow head 12/6/0.6; kDim 0.35; emphasis floor 0.15; thickness ×1.3/×1.8/×1.4; kAxisHalfLenPx 60; disc radii 4/3 px | various | SETTING | debug.physics.style.{arrowShortLen,arrowHeadLen,arrowHeadSpread,sleepingDim,emphasisFloor,subjectThicknessScale,axisHiThicknessScale,normalThicknessScale,axisHalfLenPx,anchorDiscRadius,contactDiscRadius} | DebugPhysicsStyleSettings | Game Dev | Pref-P | Live | — | N | Overlay styling |

### Input
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Input/InputActions.cpp:370 | kBtnThreshold | 0.5 | SETTING | input.pressThreshold | InputSettings | Game | Project | Live | 0.05..1 | Y | Analog-as-button and chord threshold (R2: input thresholds decide which actions fire; recorded raw input replays differently) |
| InputActions.cpp:390-391 | deadzone processor defaults min / max | 0.125 / 0.925 | SETTING | input.deadzone.defaultMin / defaultMax | InputDeadzoneSettings | PlayerSafe | Project | Live | 0..1 | Y | Standard player option; per-binding parameters still override (R2: input thresholds decide which actions fire; recorded raw input replays differently) |
| InputActions.cpp:392 | factor | 1.0 | CONSTANT | — | — | — | — | — | — | N | Identity scale |
| ArcaneClient/src/Arcane/Input/InputActions.hpp:109 | kDefaultHoldSeconds | 0.4 (s) | SETTING | input.holdSeconds | InputSettings | Game | Project | Live | 0.05..5 | Y | Interaction timing (the editor's readable text uses it too) (R2: input thresholds decide which actions fire; recorded raw input replays differently) |
| InputActions.hpp:110 | kDefaultTapSeconds | 0.2 (s) | SETTING | input.tapSeconds | InputSettings | Game | Project | Live | 0.05..2 | Y | Interaction timing (R2: input thresholds decide which actions fire; recorded raw input replays differently) |
| InputActions.cpp:1265 | kMaxTransitions | 256 | SETTING | input.maxQueuedTransitions | InputSettings | Game Dev | Project | Restart | 16..4096 | N | Queue cap (overflow is reported once) |
| ArcaneClient/src/Arcane/Input/InputRebindOperation.cpp:155-156 | rebind axis threshold | 0.5 | SETTING | input.rebind.axisThreshold | InputRebindSettings | Game Dev | Project | Live | 0.1..1 | N | Rebind sensitivity |
| InputActions.cpp:192,134,1795 | kMouseButtonCount 5; kNoGamepadToken -1; F1..F12 | — | CONSTANT | — | — | — | — | — | — | N | SDL vocabulary; sentinel |
| ArcaneClient/src/Arcane/Input/InputSnapshot.hpp:16-17,53,57 | kScancodeWords 8 (512 scancodes); kMaxKeycodesDown 16 | — | CONSTANT | — | — | — | — | — | — | N | SDL_SCANCODE_COUNT; fixed arrays in InputSnapshot, which crosses the plugin ABI |
| ArcaneClient/src/Arcane/Input/InputDevices.cpp:58,205,226,233; InputRebindOperation.cpp:107 | 15 buttons; /32767; scancode < 512 | — | CONSTANT | — | — | — | — | — | — | N | SDL ranges |
| ArcaneClient/src/Arcane/Input/LocalInputUser.cpp:222 | profile name length cap | 64 (chars) | CONSTANT | — | — | — | — | — | — | N | Filename-safety validation (security) |

### Audio
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Audio/AudioTypes.hpp:68 | sampleRate | 48000 (Hz) | SETTING | audio.sampleRate | AudioSettings | Game Dev | Project | Restart | 22050..192000 | N | Device format |
| AudioTypes.hpp:69 | channels | 2 | SETTING | audio.channels | AudioSettings | PlayerSafe | Pref-P | Restart | 1..8 | N | Speaker setup |
| AudioTypes.hpp:80 | enableDevice | false | DERIVED | — | — | — | — | — | — | N | Comes from the ClientRuntime constructor (RuntimeApp.cpp:103) |
| AudioTypes.hpp:85 | SoundLoadDesc::mode | DecodeToMemory | SETTING | audio.defaultLoadMode | AudioSettings | Game | Project | NextWorld | decode \| stream | N | Memory against streaming |
| AudioTypes.hpp:96-101 | PlayDesc bus / volume / pitch / pan / loop / startPaused | master / 1 / 1 / 0 / false / false | CONSTANT | — | — | — | — | — | — | N | Identity per-call defaults |
| ArcaneClient/src/Arcane/Audio/AudioDevice.cpp:654 | MA_SOUND_FLAG_NO_SPATIALIZATION | always | SETTING | audio.spatialization | AudioSettings | Game | Project | NextWorld | bool | N | Spatial audio is hard-wired off |
| AudioDevice.cpp:361 | kChunkFrames (null-backend pump) | 512 (frames) | CONSTANT | — (allow-list candidate) | — | — | — | — | — | N | capacity hint, no observable preference; ARC_CONSTANT marker (reconciled). Scratch-chunk size hint |

### Verify and settle harness
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneClient/src/Arcane/Host/SettleBound.hpp:32 | kSettleIntervalMs | 50 (ms) | CONSTANT | — | — | — | — | — | — | N | Test-only verification harness |
| ArcaneClient/src/Arcane/Host/VerifyReport.hpp:235-236 | kSchemaVersion / kOldestSupported | 13 / 3 | CONSTANT | — | — | — | — | — | — | N | Report format |
| ArcaneClient/src/Arcane/Host/VerifyReport.cpp:388-390, :358 | Rec.709 luma weights; /(255×3) | — | CONSTANT | — | — | — | — | — | — | N | Math |
| ArcaneClient/src/Arcane/Host/ReferenceImages.cpp:43-44,100; ArcaneClient/src/Arcane/Host/ExclusionList.cpp:34-47; ArcaneClient/src/Arcane/Host/Verdict.cpp:9 | reference paths; date format; verdict table | — | CONSTANT | — | — | — | — | — | — | N | Layout and format |

### Server (ArcaneServer)
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneServer/src/ServerConfig.hpp:22; ServerConfig.cpp:56 | fixedDtSeconds (`--fixed-dt`) | 1/60 (s); string "0.016666666666666666" | SETTING | server.tickHz | ServerSettings | Server | Project | Restart | 1..240 Hz | Y | reconciled: a server tick distinct from the client sim.fixedHz. BOTH: keep the flag and add the cvar; it sets RunLoop fixedHz (ServerApp.cpp:102) |
| Core/Config/CVarRegistry.cpp:525 | the cheats gate | false | SETTING | server.cheats | ServerSettings | Server | Project | Live | bool | N | (S6-45 amendment): spec s3.2 (S1); alias `cheats`; registered by the registry, not a ServerSettings field |
| Core/Config/CVarRegistry.cpp:525-527 | cheats permitted on this host | true | SETTING | server.cheatsAllowed | ServerSettings | Server | Project | Live | bool | N | (S6-45 amendment): spec s3.2 (S1); a dedicated host sets the Project default false |
| Core/Config/CVarRegistry.cpp:527 | a client may set Server settings | false | SETTING | server.allowClientSetServer | ServerSettings | Server | Project | Live | bool | N | (S6-45 amendment): spec s3.2 / s9 (S1) |
| ServerConfig.cpp:55 | frames | 0 | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY: run budget |
| ServerConfig.cpp:53-54,61,63 | project / plugin / report / print-engine-info | "" / false | CONSTANT | — | — | — | — | — | — | N | FLAG ONLY |
| ArcaneServer/src/ServerApp.cpp:102-103, :164, :170 | SetFixedHz / Loop().Advance step / sleep_until pacing | from fixedDt | DERIVED | — | — | — | — | — | — | Y | Follows server.tickHz (R1 split; --fixed-dt overrides) |
| ArcaneServer/src/main.cpp:125 | diag.unattended | true | CONSTANT | — | — | — | — | — | — | N | A dedicated server has no desktop session; changing it is a bug |
| main.cpp:50; ArcaneServer/src/ServerReport.hpp:24 | relaunch strip list; kSchemaVersion 1 | — | CONSTANT | — | — | — | — | — | — | N | Relaunch-line rule; report format |

### Crash reporter (ArcaneCrashReporter)
| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | audience | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneCrashReporter/src/ReporterArgs.hpp:76 | deadlineSeconds (`--deadline`) | 60 (s) | SETTING | diagnostics.reporter.deadlineSeconds | ReporterSettings | Game Dev | Project | Restart | 1..600 | N | BOTH: the flag stays; the host should pass the cvar's value through it |
| ArcaneCrashReporter/src/Symbolizer.hpp:39 | maxFramesPerThread | 64 | SETTING | diagnostics.reporter.maxFramesPerThread | ReporterSettings | Game Dev | Project | Restart | 8..8192 | N | Report depth |
| Symbolizer.hpp:45 | maxFramesFaultingThread | 8192 | SETTING | diagnostics.reporter.maxFramesFaultingThread | ReporterSettings | Game Dev | Project | Restart | 64..65536 | N | Matches UE's MaxFrames |
| Symbolizer.hpp:46 | maxThreads | 64 | SETTING | diagnostics.reporter.maxThreads | ReporterSettings | Game Dev | Project | Restart | 1..1024 | N | Report breadth |
| Symbolizer.hpp:47 | waitForEventMs | 30000 (ms) | SETTING | diagnostics.reporter.dbgengWaitMs | ReporterSettings | Game Dev | Project | Restart | 1000..300000 | N | dbgeng wait |
| ArcaneCrashReporter/src/ReporterMain.cpp:413 | log tail (crash) | 200 (lines) | SETTING | diagnostics.logTailLines | DiagnosticsSettings | Game Dev | Pref-P | Live | 0..10000 | N | Size of the log excerpt (reconciled R1: one value for the reporter and the editor crash document; the host passes it on the reporter command line) |
| ArcaneCrashReporter/src/Monitor.cpp:237 | log tail read (hang) | 512 (lines) | SETTING | diagnostics.reporter.hangLogTailLines | ReporterSettings | Game Dev | Project | Restart | 0..10000 | N | Size of the log excerpt |
| Monitor.cpp:316 | finished-view tail | 200 (lines) | DERIVED | (diagnostics.logTailLines) | — | — | — | — | — | N | Same value as the crash tail (reconciled R1) |
| ReporterMain.cpp:548 | FlushFileSinkBounded | 1000 (ms) | SETTING | diagnostics.reporter.flushTimeoutMs | ReporterSettings | Game Dev | Project | Restart | 0..10000 | N | Bounded flush |
| ReporterMain.cpp:582 | UI wait slice | 250 (ms) | SETTING | diagnostics.reporter.uiPollMs | ReporterSettings | Game Dev | Project | Restart | 10..1000 | N | Poll cadence |
| ArcaneCrashReporter/src/ReporterWindow.cpp:124 | window width | 1000 (px) | SETTING | diagnostics.reporter.windowWidth | ReporterSettings | Game | Pref-P | Restart | 320..7680 | N | Preference (S6-45 amendment): split from diagnostics.reporter.windowSize for the --window WxH flag (S6-4); scope Pref-P confirmed (ruling S6-4); range recorded from the code |
| ArcaneCrashReporter/src/ReporterWindow.cpp:124 | window height | 640 (px) | SETTING | diagnostics.reporter.windowHeight | ReporterSettings | Game | Pref-P | Restart | 240..4320 | N | as diagnostics.reporter.windowWidth (S6-45 amendment) |
| ReporterWindow.cpp:130 | WaitUntilReady | 5000 (ms) | SETTING | diagnostics.reporter.windowReadyMs | ReporterSettings | Game Dev | Project | Restart | 100..60000 | N | Timeout |
| ReporterWindow.cpp:717 | copy-confirmation flash | 0.75 (s) | SETTING | ui.copyFlashSeconds | UiSettings | Game Dev | Pref-M | Live | 0..5 | N | UI feedback (reconciled R1: shared with the editor; the reporter receives it on its command line) |
| ReporterWindow.cpp:350,355,370-371,611 | layout metrics 12/20/28/8, 320/200, 150/72, 320 | DIP px | DERIVED | — | — | — | — | — | — | N | Layout; base px x 1 with an ARC_CONSTANT base (s16.11 applied to the reporter, which has no UI scale; was diagnostics.reporter.layout.*) (reconciled R1) |
| ReporterWindow.cpp:33,541,563,566,596,635 | font sizes 10pt / 16 / 16 / 15 / 18 / 15 | pt / px | DERIVED | (editor.appearance.font*) | — | — | — | — | — | N | The "editor-styled" reporter should follow the editor's font settings |
| ArcaneCrashReporter/src/WarpImGui.cpp:169 | panel colour | 30/255 grey | DERIVED | (editor theme token) | — | — | — | — | — | N | Should follow the theme |
| ReporterWindow.cpp:349,402,480 | 96 DPI baseline; 72 points per inch | — | CONSTANT | — | — | — | — | — | — | N | Windows DPI and point math |
| WarpImGui.cpp:54 | RefreshRate 60/1 | — | CONSTANT | — | — | — | — | — | — | N | DXGI windowed swapchain field; ignored |
| ArcaneCrashReporter/src/HangSession.cpp:21; ArcaneCrashReporter/src/MonitorRule.cpp:72 | host exit codes 10 / 12 / 13 | — | CONSTANT | — | — | — | — | — | — | N | Process protocol with the watchdog |
| ReporterArgs.hpp:24-46; ReporterWindow.hpp:38-40; ArcaneCrashReporter/src/ReportView.hpp:76 | exit codes; control IDs | — | CONSTANT | — | — | — | — | — | — | N | Protocol and IDs |
| ArcaneCrashReporter/src/ReportView.cpp:189-205; ArcaneCrashReporter/src/SymbolizedText.cpp:154-163; ArcaneCrashReporter/src/Symbolizer.cpp:69,140,157,174,253 | timestamp format; report text headers; dbgeng buffers 1024 / 4096 | — | CONSTANT | — | — | — | — | — | — | N | Formats that get parsed back; API buffer sizes |

### Thumbnails and preview lights

*Reconciled 2026-10-03: these rows duplicated Part 3 (MaterialPreviewHarvester.cpp is in ArcaneEditor) with conflicting names. Part 3's rows are canonical; see the Reconciliation table.*

### Should NOT be exposed
- **Shader and HLSL contracts**
  - kBindlessCapacity 256 (MeshNode.hpp:565, must equal mesh.hlsl:155).
  - kMeshCullThreads 64 (MeshCullNode.hpp:29, must equal mesh_cull.hlsl:60 numthreads).
  - kVulkanBindingOffsets 0/128/256/384 (NriDevice.cpp:72-86).
  - kMaxSelectedIds 64 (PickOutlineNodes.hpp:489).
  - GpuSceneTypes instance flag bits (GpuSceneTypes.hpp:30-38).
- **File, report and wire formats**
  - GPU dump layout (IGpuCrashBackend.hpp:58-61).
  - VerifyReport kSchemaVersion 13 (VerifyReport.hpp:235) and ServerReport kSchemaVersion 1 (ServerReport.hpp:24).
  - Symbolized-text headers (SymbolizedText.cpp:154-163) and the report timestamp format (ReportView.cpp:189-205).
- **Process protocols and IDs**
  - Watchdog exit codes 10/12/13 (HangSession.cpp:21, MonitorRule.cpp:72).
  - Reporter exit codes (ReporterArgs.hpp:24-46).
  - Splash window-message IDs (BootSplashWindow.cpp:42-43).
  - Shader-pair IDs (0x2000, 0x3000, 0x4000-0x4101, 0x5000, 0x6000).
- **ABI**
  - D3D12SDKVersion 619 (ArcaneRuntime/src/main.cpp:24).
  - InputSnapshot's fixed arrays (InputSnapshot.hpp:16-17).
  - NriPipelineCache kMaxColorAttachments 4 (NriPipelineCache.hpp:102).
  - RenderGraph handle bits 24/8 (RenderGraph.hpp:900-903).
- **Hardware and graphics-API minimums, limits and alignment**
  - D3D_FEATURE_LEVEL_12_0 (DeviceCreationD3D12.cpp:443).
  - Vulkan 1.3 (DeviceCreationVulkan.cpp:45).
  - Shader model 6.5 profiles (ShaderConventions.hpp:36-38).
  - 256-byte constant-buffer windows (kFrameCbMaxBytes; kRingAlign 16 and kVisibilityRegionAlign 256 at GpuScene.cpp:31,36).
  - queueNum 1 (NriDevice.cpp:235).
- **Math identities**
  - kPi, kTau and kEps (Gizmo.cpp:18-21).
  - FNV offset and prime (Batch2DNode.cpp:103-104, ShaderCompiler.cpp:204-205).
  - Rec.709 luma weights (VerifyReport.cpp:388-390).
  - Unity volume, pitch and pan (AudioTypes.hpp:97-99).
  - mipMax 16 and depth clear 1.0.
- **Security and permissions**
  - The runtime console's Permission::Player (RuntimeApp.cpp:1568, RuntimeFrame.cpp:360, ProjectBoot.hpp:207).
  - The profile-name length cap of 64 (LocalInputUser.cpp:222).
- **Test-only and automation-only values**
  - Fault-injector constants (NriDiagnostics.cpp:371-379).
  - kSettleIntervalMs 50 (SettleBound.hpp:32) and the settle-timeout default of 5000 (HostConfig.hpp:169).
  - Run-input flags such as `--frames`, `--probe` and `--compare` (HostConfig.cpp:86-145).
- **Changes that would be bugs, or a correctness policy**
  - DRED always on (DeviceCreationD3D12.cpp:428).
  - enableMemoryZeroInitialization must stay false (NriDevice.cpp:266).
  - Sprite clamp addressing (Batch2DNode.cpp:287-288).
  - BGRA8 offscreen capture format (NriGraphContext.hpp:297).
  - ArcaneServer unattended = true (ArcaneServer/src/main.cpp:125).
  - The 1 ms OS-floor sleeps (NriSwapChain.cpp:39, RuntimeFrame.cpp:756).
  - The glTF default alphaCutoff of 0.5 (GpuSceneSync.hpp:73).
- **Sentinels**
  - 0xFFFFFFFF and size_t(-1) markers (RenderGraph.hpp:77,460-461,904,909; BindlessTable.hpp:52; Batch2DNode.hpp:155; NriPipelineCache.hpp:107).
  - kInvalidMaterialId 0xFFFF (Batcher2D.hpp:210).

### Notes
- **Only two cvars exist in all four directories:** `render.meshCull` (MeshCullNode.cpp:21) and `diagnostics.drawMarkers` (GpuInstrumentation.cpp:27). Everything else in the tables is hard-coded.
- **Gizmo snap steps can't be changed:** the editor only ever sets `GizmoSnap::enabled` (EditorAppFrame.cpp:1303-1304). Nobody can change the 0.5 m, 15° and 0.1 steps (Gizmo.hpp:95-97). This is the clearest missing preference.
- **Debug HUD always shows:** the "ArcaneRuntime" debug HUD is drawn every frame in every configuration, including Dist (RuntimeApp.cpp:810, RuntimeFrame.cpp:302). Shipped games show it. Because goldens include it, the cvar default must stay on unless someone re-blesses on purpose.
- **Physics debug lengths look like pre-metres leftovers:** kManifoldNormalLen 20 and kNormalLen 28 are labelled "world units" (PhysicsDebugDraw.cpp:138, :747). With metres as the unit, those arrows are 20 m and 28 m long. The contactMarkerSize comment (PhysicsDebugDraw.hpp:72) still cites pixelsPerMeter=100. Convert byte-identically, but treat these as bug candidates.
- **The same default is written in several places and will drift:**
  - **Fixed step 1/60:** HostConfig.hpp:73; HostConfig.cpp:96 as the string "0.0166666666666666666"; ServerConfig.hpp:22; ServerConfig.cpp:56 as "0.016666666666666666" (one digit fewer); OffscreenImGuiLayer.hpp:58; plus Core's RunLoop 60 Hz.
  - **Shader debounce 0.2:** RuntimeApp.cpp:311, ShaderCompiler.cpp:405 and ShaderCompiler.hpp:111.
  - **15 s fence/poll window:** in three files, silently tied to Core's 8 s gpuStallSeconds.
  - **kHangMainSeconds 15:** tied to Core's 12 s hangSeconds.
  - **ProjectBoot kStride:** copied by hand into EditorAppProject.cpp.
  - **Preview light:** the triple appears twice, plus a third, different triple in MeshDocument.
  - **UndoLimits:** its defaults repeat the editor.undo.* cvar defaults.
- **Frames in flight is the biggest conversion:** kSwapchainFramesInFlight has 167 references and sizes 36 fixed arrays, and every descriptor-pool size depends on it. Converting it needs a kMaxFramesInFlight CONSTANT for storage plus a runtime render.framesInFlight setting capped by it.
- **Project identity is hard-coded:**
  - Every project's base input context is "demo" (ProjectBoot.hpp:120).
  - Every game's window is titled "Arcane Runtime" (GpuContext.cpp:25), even though the reporter's product name already comes from the project.
  - Every game boots on the engine logo splash (ArcaneRuntime/src/main.cpp:212).
- **Almost nothing exists yet for players (PlayerSafe):**
  - There is no fullscreen or display-mode setting, no resolution beyond window size, and no master volume.
  - Spatial audio is forced off (AudioDevice.cpp:654).
  - The natural PlayerSafe candidates are backend, vsync, window size, anisotropy, adapter, tearing, audio channels and deadzones.
- **Shader compiles can't be debugged at source level:** compiles pass no `-O` or `-Zi` (ShaderCompiler.cpp:270-288) and run on a single worker thread.
- **Release can't turn on validation:** Release and Dist builds have no way to enable GPU validation without recompiling (RenderDeviceDesc.hpp:20-24, NriGraphContext.cpp:175-183).
- **Sprite-texture overflow passes silently:** going past kMaxSpriteTextures turns sprites into flat tint, logs one ERROR, and still exits 0 (Batch2DNode.hpp:255-262). That is a strong reason to expose it.
- **Defaults that move goldens:** these must keep identical defaults during the sweep and only change with a deliberate re-bless:
  - window 1280x720
  - kCanvasClear
  - the mesh default light triple
  - kPickSupersample
  - outline colours and widths
  - canvas format
  - shader debounce (it moves the settle frame)
  - runtime HUD visibility
  - grid colours
  - thumbnail size, light, time and FOV
- **Thumbnails are not in the four audited directories:** they live in ArcaneEditor (MaterialPreviewHarvester.cpp:44, 55, 1237, 1257-1259). The only in-scope part is MeshSceneDesc's default light at MeshNode.hpp:297-299.
- **The crash reporter has no settings store:** ArcaneCrashReporter is a separate exe without a CVarRegistry. Its settings would have to travel from the host's diagnostics category through command-line arguments, the way `--deadline` already does.
- **No network settings yet:** ArcaneServer has no ports, timeouts or rate limits. Tick rate is its only tunable, so the `net.*` category is empty in this scope.
- **New category names:** the proposals use `audio.*`, `app.splash.*`, `boot.*`, `console.*`, `runtime.*`, `sim.*`, `debug.physics.*` and `assets.thumbnail.*`, which spec section 5.2 does not list. The spec should either add them or fold them into existing ones.
- **Capacity hints:** reserve sizes and I/O chunks (MeshNode.hpp:645, CommandStack.cpp:111, ComponentEditCommand.cpp:34, ShaderCompiler.cpp:244, AudioDevice.cpp:361) are marked SETTING Dev only because of the "when in doubt" rule. I recommend putting them on the reviewed-CONSTANT allow-list rather than making them cvars.

---

## Part 3: ArcaneEditor and ArcaneHub

## Settings inventory: ArcaneEditor + ArcaneHub (read-only audit, 2026-10-03)

Paths are relative to `D:\dev\starworks\Arcane\ArcaneEditor\src\` unless they start with `ArcaneHub/`, `ArcaneClient/` or `ArcaneCore/`. Spec: `docs/superpowers/specs/2026-10-03-settings-and-cvar-completion-design.md` (s3, s6, s7, s10). The audit only read files; nothing was edited, built or run.

Column key:
- **aud**: audience.
- **scope**: `Pref-M` = Preferences, machine-wide (EditorUser rung). `Pref-P` = Preferences, per project (User rung under `Saved/Config`). `Project` = Project rung.
- **det**: Y if changing the value changes a simulation's outcome or replays.
- **—**: not applicable.

---

### Theme

The 31 tokens in `Widgets/EditorTheme.hpp`, then the theme's literal alphas, then the domain palettes that live outside `EditorTheme.hpp`.

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Widgets/EditorTheme.hpp:74 | kChromeDeep | #0c0c0c (sRGB) | SETTING | editor.theme.chromeDeep | EditorThemeSettings | Editor | Pref-M | Live | Color | N | title bars, scrollbar track |
| Widgets/EditorTheme.hpp:75 | kChrome | #191919 | SETTING | editor.theme.chrome | EditorThemeSettings | Editor | Pref-M | Live | Color | N | menu bar, popups, table header |
| Widgets/EditorTheme.hpp:81 | kPanel | #1e1e1e | SETTING | editor.theme.panel | EditorThemeSettings | Editor | Pref-M | Live | Color | N | base surface |
| Widgets/EditorTheme.hpp:82 | kPanelRaised | #2a2a2a | SETTING | editor.theme.panelRaised | EditorThemeSettings | Editor | Pref-M | Live | Color | N | row/tab hover |
| Widgets/EditorTheme.hpp:88 | kWell | #121212 | SETTING | editor.theme.well | EditorThemeSettings | Editor | Pref-M | Live | Color | N | input wells |
| Widgets/EditorTheme.hpp:89 | kWellHovered | #181818 | SETTING | editor.theme.wellHovered | EditorThemeSettings | Editor | Pref-M | Live | Color | N | well hover |
| Widgets/EditorTheme.hpp:90 | kWellActive | #1c1c1c | SETTING | editor.theme.wellActive | EditorThemeSettings | Editor | Pref-M | Live | Color | N | well active, checked box |
| Widgets/EditorTheme.hpp:94 | kButton | #2f2f2f | SETTING | editor.theme.button | EditorThemeSettings | Editor | Pref-M | Live | Color | N | buttons, scrollbar grab |
| Widgets/EditorTheme.hpp:95 | kButtonHovered | #3d3d3d | SETTING | editor.theme.buttonHovered | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:96 | kButtonActive | #4b4b4b | SETTING | editor.theme.buttonActive | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:101 | kSelection | #2e4053 | SETTING | editor.theme.selection | EditorThemeSettings | Editor | Pref-M | Live | Color | N | Header, TextSelectedBg, DockingPreview |
| Widgets/EditorTheme.hpp:111 | kAccent | #5b7fa6 | SETTING | editor.theme.accent | EditorThemeSettings | Editor | Pref-M | Live | Color | N | contrast bars pinned by EditorThemeContrastTest |
| Widgets/EditorTheme.hpp:112 | kAccentHovered | #6386ad | SETTING | editor.theme.accentHovered | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:113 | kAccentActive | #52769c | SETTING | editor.theme.accentActive | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:118 | kToggleOn | = kAccent | DERIVED | — | — | — | — | — | — | N | alias of accent |
| Widgets/EditorTheme.hpp:119 | kToggleOnHovered | = kAccentHovered | DERIVED | — | — | — | — | — | — | N | alias |
| Widgets/EditorTheme.hpp:120 | kToggleOnActive | = kAccentActive | DERIVED | — | — | — | — | — | — | N | alias |
| Widgets/EditorTheme.hpp:126 | kText | #e0e0e0 | SETTING | editor.theme.text | EditorThemeSettings | Editor | Pref-M | Live | Color | N | contrast-checked pair |
| Widgets/EditorTheme.hpp:127 | kTextDim | #8e8e8e | SETTING | editor.theme.textDim | EditorThemeSettings | Editor | Pref-M | Live | Color | N | 5.09:1 on kPanel, pinned by test |
| Widgets/EditorTheme.hpp:128 | kBorder | #0d0d0d | SETTING | editor.theme.border | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:129 | kSeparator | #333333 | SETTING | editor.theme.separator | EditorThemeSettings | Editor | Pref-M | Live | Color | N | also the dock splitter |
| Widgets/EditorTheme.hpp:130 | kSeparatorHot | #4a4a4a | SETTING | editor.theme.separatorHot | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:131 | kSeparatorHeld | #6e6e6e | SETTING | editor.theme.separatorHeld | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:137 | kGrab | #9a9a9a | SETTING | editor.theme.grab | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:138 | kGrabActive | #c8c8c8 | SETTING | editor.theme.grabActive | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:139 | kCheck | #d4d4d4 | SETTING | editor.theme.check | EditorThemeSettings | Editor | Pref-M | Live | Color | N | also TextLink |
| Widgets/EditorTheme.hpp:147 | kAmber | (1,0.65,0.10) | SETTING | editor.theme.amber | EditorThemeSettings | Editor | Pref-M | Live | Color | N | drop target, histogram, graph selection border |
| Widgets/EditorTheme.hpp:148 | kAmberLight | (1,0.78,0.35) | SETTING | editor.theme.amberLight | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:155 | kError | #e65959 | SETTING | editor.theme.error | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:159 | kWarning | #f2c44d | SETTING | editor.theme.warning | EditorThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/EditorTheme.hpp:163 | kNone | (0,0,0,0) | CONSTANT | — | — | — | — | — | — | N | "draw nothing" identity; any change is a bug |
| Widgets/EditorTheme.hpp:193 | TitleBgCollapsed alpha | kChromeDeep×0.75 | DERIVED | — | — | — | — | — | — | N | token × fixed alpha |
| Widgets/EditorTheme.hpp:229-231 | ResizeGrip* alphas | 0.20/0.55/0.85 | DERIVED | — | — | — | — | — | — | N | token × fixed alpha |
| Widgets/EditorTheme.hpp:249 | TabDimmedSelectedOverline | accent×0.45 | SETTING | editor.theme.unfocusedOverlineAlpha | EditorThemeSettings | Editor Dev | Pref-M | Live | 0..1 | N | focus-cue strength |
| Widgets/EditorTheme.hpp:251 | DockingPreview | selection×0.70 | DERIVED | — | — | — | — | — | — | N | token × alpha |
| Widgets/EditorTheme.hpp:271 | TableRowBgAlt | white @0.03 | SETTING | editor.theme.rowStripe | EditorThemeSettings | Editor | Pref-M | Live | Color | N | stripe strength is taste |
| Widgets/EditorTheme.hpp:274 | TextSelectedBg | selection×0.80 | DERIVED | — | — | — | — | — | — | N | token × alpha |
| Widgets/EditorTheme.hpp:277 | DragDropTarget | amber×0.90 | DERIVED | — | — | — | — | — | — | N | token × alpha |
| Widgets/EditorTheme.hpp:282 | NavWindowingHighlight | text×0.70 | DERIVED | — | — | — | — | — | — | N | token × alpha |
| Widgets/EditorTheme.hpp:285-286 | NavWindowingDimBg / ModalWindowDimBg | (0.02,0.02,0.02,0.55) | SETTING | editor.theme.modalDim | EditorThemeSettings | Editor | Pref-M | Live | Color | N | a light theme needs another value |
| Widgets/EditorWidgets.cpp:243-247 | kAxisBarColors | X #c44036, Y #60a63a, Z #3a7ac4 | SETTING | editor.theme.axis{X,Y,Z} | EditorThemeSettings | Editor | Pref-M | Live | Color | N | domain colour; colour-blind users (reconciled R1: the canonical axis tokens; S5-2 review decision 1 option A: only the inspector bars derive from them, the grid and gizmo axis colours stay values marked ARC_CONSTANT("pending axis unification re-bless")) |
| Widgets/EditorWidgets.cpp:266-268 | kHeaderBand* | (48,48,52)/(58,58,64)/(66,66,73) | SETTING | editor.theme.headerBand{,Hovered,Active} | EditorThemeSettings | Editor | Pref-M | Live | Color | N | Inspector category bands |
| Widgets/EditorWidgets.cpp:315 | kPillAmberBorder | #7a5a20 | SETTING | editor.theme.actingOnFrame | EditorThemeSettings | Editor | Pref-M | Live | Color | N | comment: promote on third use |
| Widgets/EditorWidgets.cpp:319-322 | kPillScheme{Blue,Violet}{Border,Text} | #3a4a5c/#9fb3c8/#4a3a5c/#b8a3c8 | SETTING | editor.theme.inputPill.{blueBorder,blueText,violetBorder,violetText} | EditorThemeInputPillSettings | Editor Dev | Pref-M | Live | Color | N | spec-pinned hexes |
| Widgets/ColorPickerPopup.cpp:46 | kMarkers[0] (R) | channel marker | SETTING | editor.theme.channelR | EditorThemeSettings | Editor | Pref-M | Live | Color | N | domain colour (S6-45 amendment): replaces editor.theme.channelMarkers (Color x4), one name per value; Editor, not Editor Dev: a theme token like every other editor.theme.* colour (it travels in the .arctheme presets) |
| Widgets/ColorPickerPopup.cpp:46 | kMarkers[1] (G) | channel marker | SETTING | editor.theme.channelG | EditorThemeSettings | Editor | Pref-M | Live | Color | N | domain colour (S6-45 amendment): replaces editor.theme.channelMarkers (Color x4), one name per value; Editor, not Editor Dev: a theme token like every other editor.theme.* colour (it travels in the .arctheme presets) |
| Widgets/ColorPickerPopup.cpp:46 | kMarkers[2] (B) | channel marker | SETTING | editor.theme.channelB | EditorThemeSettings | Editor | Pref-M | Live | Color | N | domain colour (S6-45 amendment): replaces editor.theme.channelMarkers (Color x4), one name per value; Editor, not Editor Dev: a theme token like every other editor.theme.* colour (it travels in the .arctheme presets) |
| Widgets/ColorPickerPopup.cpp:46 | kMarkers[3] (W) | channel marker | SETTING | editor.theme.channelW | EditorThemeSettings | Editor | Pref-M | Live | Color | N | domain colour (S6-45 amendment): replaces editor.theme.channelMarkers (Color x4), one name per value; Editor, not Editor Dev: a theme token like every other editor.theme.* colour (it travels in the .arctheme presets) |
| Documents/ShaderEditorDocument.cpp:346 | kCanvasColor | = Theme::kPanel | DERIVED | — | — | — | — | — | — | N | token |
| Documents/ShaderEditorDocument.cpp:351-355 | kNodeBody/Title/Border/TitleText/BadgeText | #2d2d30, #232326, … | SETTING | editor.theme.graph.{nodeBody,nodeTitle,nodeBorder,nodeTitleText,nodeBadgeText} | GraphThemeSettings | Editor | Pref-M | Restart | Color | N | latched by ApplyGraphCanvasStyle at canvas creation |
| Documents/ShaderEditorDocument.cpp:360-361 | kGroupBg/BorderColor | α 0.25 / 0.60 | SETTING | editor.theme.graph.{groupBg,groupBorder} | GraphThemeSettings | Editor | Pref-M | Restart | Color | N | — |
| Documents/ShaderEditorDocument.cpp:380 | kPinTextureColor | red-orange | SETTING | editor.theme.graph.pinTexture | GraphThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Documents/ShaderGraphPinTypes.hpp:31-34 | kPin{Scalar,Vec2,Vec4,Dynamic}Color | azure/green/magenta/gray | SETTING | editor.theme.graph.{pinScalar,pinVec2,pinVec4,pinDynamic} | GraphThemeSettings | Editor | Pref-M | Live | Color | N | legend shows the same values |
| Documents/ShaderGraphCategoryColors.hpp:22-29 | category header colours | 7 hexes | SETTING | editor.theme.graph.category.{input,math,vector,procedural,output,utility,uncategorized} | GraphCategoryThemeSettings | Editor | Pref-M | Live | Color | N | contrast test s5.1.11 pins them |
| Widgets/GraphCanvasStyle.hpp:74-75 | kGraphGrid{Minor,Major}Color | α 0.55 / 0.90 | SETTING | editor.theme.graph.grid{Minor,Major} | GraphThemeSettings | Editor | Pref-M | Live | Color | N | — |
| Widgets/GraphCanvasStyle.hpp:87 | kGraphNodeSelBorderColor | = kAmber | DERIVED | — | — | — | — | — | — | N | token |
| Widgets/GraphCanvasStyle.hpp:88 | kGraphNodeHovBorderColor | (0.25,0.70,1) | SETTING | editor.theme.graph.hoverBorder | GraphThemeSettings | Editor | Pref-M | Restart | Color | N | — |
| Panels/AssetGraphPanel.cpp:462-465 | kGraph{Canvas,NodeBody,NodeTitle,NodeBorder} | = kWell/kPanel/kChrome/kBorder | DERIVED | — | — | — | — | — | — | N | tokens |
| Panels/AssetGraphPanel.cpp:1168-1169 | kGraphLegend{Edge,UsedBy}Color | #5c5c5c / #4a4a4a | SETTING | editor.theme.assetGraph.{legendEdge,legendUsedBy} | EditorThemeAssetGraphSettings | Editor Dev | Pref-M | Live | Color | N | — |
| Panels/AssetPanelModel.hpp:338-351 | KindAccentRgb | 7 kind hexes | SETTING | editor.theme.assetKind.{texture,material,mesh,sprite,scene,inputActions,model} | EditorThemeAssetKindSettings | Editor | Pref-M | Live | Color | N | data colour |
| Viewport/ViewportGrid.hpp:71 | kGridLineRgb | 0.5 gray | SETTING | editor.viewport.grid.lineColor | EditorGridSettings | Editor | Pref-M | Live | Color | N | — |
| Viewport/ViewportGrid.hpp:72-73 | kGridAxis{X,Y}Color | red / green @0.9 | CONSTANT | — | — | — | — | — | — | N | ARC_CONSTANT("pending axis unification re-bless"): the 2D grid axis colours stay values until a post-sweep task unifies them with the gizmo and inspector axis tokens in one deliberate re-bless (S5-2 review, decision 1 option A; was DERIVED from editor.theme.axis{X,Y}, ruling I4) |
| App/EditorAppFrame.cpp:1959 | scene-camera frame colour | (0.45,0.62,0.78,0.75) | SETTING | editor.theme.viewport.cameraFrame | EditorThemeViewportSettings | Editor | Pref-M | Live | Color | N | — |
| Panels/EditorPanels.cpp:1243 | console info text | gray 0.80 | DERIVED | — | — | — | — | — | — | N | should be Theme::kText (drift: 0.80 ≠ 0.878) |
| Panels/EditorPanels.cpp:1348 | cvar reply error colour | (1.0,0.45,0.45) | DERIVED | — | — | — | — | — | — | N | should be Theme::kError (drift: ≠ #e65959) |

### Fonts/Style

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Widgets/EditorFonts.hpp:23 | InstallEditorFonts sizePx | 16 px | SETTING | editor.ui.fontSize | EditorUiSettings | Editor | Pref-M | Live (atlas rebuilt next frame) | 10..32 | N | spec s7.3 |
| Widgets/EditorFonts.cpp:76 | UI face | Inter_18pt-Regular.ttf | SETTING | editor.ui.fontFamily | EditorUiSettings | Editor | Pref-M | Live | font hint | N | spec s7.3 |
| Widgets/EditorFonts.cpp:78 | alternate face | Roboto-Regular.ttf | SETTING | editor.ui.altFontFamily | EditorUiSettings | Editor Dev | Pref-M | Live | font hint | N | pushable alternate |
| Widgets/EditorFonts.cpp:80 | mono face | JetBrainsMono-Regular.ttf | SETTING | editor.ui.monoFontFamily | EditorUiSettings | Editor | Pref-M | Live | font hint | N | spec s7.3 |
| Widgets/EditorFonts.cpp:86 | brand face | AldotheApache.ttf | CONSTANT | — | — | — | — | — | — | N | brand wordmark, not a preference |
| Widgets/EditorFonts.cpp:57 | GlyphMinAdvanceX | = sizePx | DERIVED | — | — | — | — | — | — | N | monospace icon cell |
| Widgets/EditorFonts.cpp:58 | icon GlyphOffset.y | 3 px | DERIVED | — | — | — | — | — | — | N | should scale with fontSize (baseline nudge) |
| — (not present) | UI scale | 1.0 | SETTING | editor.ui.scale, editor.ui.followDpi | EditorUiSettings | Editor | Pref-M | Live | 0.75..2.0 | N | new per spec s7.3; no ScaleAllSizes call exists today |
| Widgets/EditorTheme.hpp:297 | style.FrameBorderSize | 1 px | SETTING | editor.ui.frameBorderSize | EditorUiSettings | Editor | Pref-M | Live | 0..2 | N | the inset well edge |
| Widgets/EditorTheme.hpp:305 | DockingNodeHasCloseButton | false | SETTING | editor.ui.dockNodeCloseButton | EditorUiSettings | Editor Dev | Pref-M | Live | bool | N | taste |
| Widgets/EditorTheme.hpp:313 | TabBarOverlineSize | 2 px | SETTING | editor.ui.tabOverlineSize | EditorUiSettings | Editor | Pref-M | Live | 0..4 | N | — |
| Widgets/EditorTheme.hpp:319 | DisabledAlpha | 0.45 | SETTING | editor.ui.disabledAlpha | EditorUiSettings | Editor | Pref-M | Live | 0.2..1 | N | tuned against kTextDim |
| Widgets/EditorTheme.hpp:326 | TabRounding | 2 px | SETTING | editor.ui.tabRounding | EditorUiSettings | Editor | Pref-M | Live | 0..8 | N | user request |
| Panels/AssetGraphPanel.cpp:399 | kGraphHeaderFontPx | 14 px | DERIVED | — | — | — | — | — | — | N | should be fontSize × ratio (absolute today; ignores scale) |
| Panels/AssetGraphPanel.cpp:400 | kGraphMetaFontPx | 13 px | DERIVED | — | — | — | — | — | — | N | same |
| Panels/AssetGraphPanel.cpp:519 | kGraphLabelFontPx | 12 px | DERIVED | — | — | — | — | — | — | N | same |
| Widgets/GraphLegend.hpp:32 | kGraphLegendFontPx | 13 px | DERIVED | — | — | — | — | — | — | N | same |
| Panels/AssetBrowserPanel.cpp:708 | kBadgeFontSize | 10 px | DERIVED | — | — | — | — | — | — | N | same |
| Panels/AssetPanelCommon.cpp:298 | PushFont size | 12 px | DERIVED | — | — | — | — | — | — | N | same |
| Panels/AssetStatusPanel.cpp:153, :338 | PushFont size | 13 px | DERIVED | — | — | — | — | — | — | N | same |
| Widgets/EditorWidgets.cpp:736 | pill font | 12 px | DERIVED | — | — | — | — | — | — | N | same |
| Widgets/EditorWidgets.cpp:927 | stat number font | 24 px | DERIVED | — | — | — | — | — | — | N | same |
| Widgets/EditorWidgets.cpp:943 | stat caption font | 13 px | DERIVED | — | — | — | — | — | — | N | same |
| Documents/CrashReportDocument.cpp:182 | heading scale | FontSizeBase×1.35 | DERIVED | — | — | — | — | — | — | N | already relative |
| Panels/EditorPanels.cpp:768 | toolbar logo height | btnH×1.35 | SETTING | editor.ui.toolbar.logoScale | EditorUiToolbarSettings | Editor Dev | Pref-M | Live | 1..2 | N | — |
| Panels/EditorPanels.cpp:794 | brand wordmark size | logoH×0.80 | SETTING | editor.ui.toolbar.brandScale | EditorUiToolbarSettings | Editor Dev | Pref-M | Live | 0.5..1 | N | — |
| Panels/EditorPanels.cpp:781, :788, :828, :1018, :770 | toolbar pads / gaps | 8, 8, 12, 12, 3 px | DERIVED | — | — | — | — | — | — | N | should scale with editor.ui.scale; base px x editor.ui.scale (s16.11; was editor.ui.toolbar.*) |
| Panels/EditorPanels.cpp:816-817 | kTransportGap, kCaretPadX | 2, 2 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.ui.toolbar.transportGap) |
| Widgets/EditorWidgets.hpp:240 | kPillLineHeight | 16 px | DERIVED | — | — | — | — | — | — | N | should derive from fontSize |
| Widgets/EditorWidgets.hpp:309 | default rowHeight | 24 px | DERIVED | — | — | — | — | — | — | N | = kTableRowHeight; should derive from fontSize |
| Widgets/EditorWidgets.cpp:372, :911 | kCardFramePadding, kPad | 8 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.ui.cardPadding) |
| Widgets/EditorWidgets.cpp:971-975 | distribution bar: height, segment gap, swatch, legend gaps | 12, 2, 8, 6, 14 px | DERIVED | — | — | — | — | — | — | N | spec §11.2 pinned; base px x editor.ui.scale (s16.11; was editor.ui.distBar.*) |
| Widgets/EditorWidgets.cpp:1144-1148 | activity-feed dot / gaps | 7, 8, 6, 2 px | DERIVED | — | — | — | — | — | — | N | spec §11.2 pinned; base px x editor.ui.scale (s16.11; was editor.ui.feed.*) |
| Widgets/EditorWidgets.cpp:1324 | link underline offset | Descent×0.20 | DERIVED | — | — | — | — | — | — | N | font metric |
| App/EditorAppFrame.cpp:3442 | modal wrap width | FontSize×30 | DERIVED | — | — | — | — | — | — | N | relative |
| App/EditorAppFrame.cpp:3446, :3536, :3580, :3587 | modal button widths | 120, 140/90, 90, 90 px | DERIVED | — | — | — | — | — | — | N | should scale; base px x editor.ui.scale (s16.11; was editor.ui.modalButtonWidth) |

### Viewport/Camera/Gizmo

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Viewport/ViewportSettings.hpp:52 | showGrid | true | SETTING | editor.viewport.showGrid | EditorViewportSettings | Editor | Pref-P | Live | bool | N | in imgui.ini today (s10.2) |
| Viewport/ViewportSettings.hpp:53 | gridPlane | XZ (enum) | SETTING | editor.viewport.gridPlane | EditorViewportSettings | Editor | Pref-P | Live | {XZ, XY} | N | ini today |
| Viewport/ViewportSettings.hpp:54 | gizmoSize | 1.0 (×) | SETTING | editor.gizmo.size | EditorGizmoSettings | Editor | Pref-P | Live | 0.1..10 (UI 0.5..3) | N | ini today |
| Viewport/ViewportSettings.hpp:59-60 | kMin/MaxGizmoSize | 0.1 / 10 | DERIVED | — | — | — | — | — | — | N | the range of editor.gizmo.size |
| Viewport/ViewportSettings.hpp:63-64 | kMin/MaxFovYDeg | 1 / 179 deg | CONSTANT | — | — | — | — | — | — | N | projection finite only inside (0,180) |
| Viewport/ViewportSettings.hpp:66-67 | kMin/MaxSpeedScalar | 0.01 / 100 | DERIVED | — | — | — | — | — | — | N | range of editor.camera.speedScalar; duplicated at EditorCamera.cpp:114 |
| Viewport/ViewportSettings.hpp:69 | kMaxPitchDeg | 90−1e-3 deg | CONSTANT | — | — | — | — | — | — | N | Right()/Up() are NaN at ±90 |
| Panels/EditorPanels.cpp:1500 | FOV slider range | 20..120 deg | DERIVED | — | — | — | — | — | — | N | UI range metadata of editor.camera.fovYDeg |
| Panels/EditorPanels.cpp:1506 | gizmo slider range | 0.5..3 | DERIVED | — | — | — | — | — | — | N | UI range metadata |
| Panels/EditorPanels.cpp:1490 | settings popup item width | FontSize×10 | DERIVED | — | — | — | — | — | — | N | relative |
| Viewport/EditorCamera.hpp:90 | Orbit3D::fovYDeg | 60 deg | SETTING | editor.camera.fovYDeg | EditorCameraSettings | Editor | Pref-P | Live | 20..120 | N | lives in the ini Orbit= pose line today |
| Viewport/EditorCamera.hpp:138 | speedScalar | 1.0 | SETTING | editor.camera.speedScalar | EditorCameraSettings | Editor | Pref-P | Live | 0.01..100 (log) | N | ini Speed= |
| Viewport/EditorCamera.hpp:77 | Ortho2D::halfHeight default | 5 m | SETTING | editor.camera.default2DHalfHeight | EditorCameraSettings | Editor Dev | Pref-P | NextWorld | 0.01..1e6 | N | fresh-camera framing |
| Viewport/EditorCamera.hpp:87-89 | default yaw / pitch / distance | −30°, 30°, 10 m | SETTING | editor.camera.default3D{Yaw,Pitch,Distance} | EditorCameraSettings | Editor Dev | Pref-P | NextWorld | — | N | fresh-camera pose |
| Viewport/EditorCamera.hpp:113-114 | kMin/MaxHalfHeight | 0.01 / 1e6 m | SETTING | editor.camera.ortho{Min,Max}HalfHeight | EditorCameraSettings | Editor Dev | Pref-P | Live | >0 | N | UE MIN/MAX_ORTHOZOOM |
| Viewport/EditorCamera.hpp:117 | kWheelStep | 1.12 (×/tick) | SETTING | editor.camera.wheelZoomStep | EditorCameraSettings | Editor | Pref-P | Live | 1.01..2 | N | zoom/dolly feel |
| Viewport/EditorCamera.hpp:122 | kFrameFill | 0.9 | SETTING | editor.camera.frameFill | EditorCameraSettings | Editor | Pref-P | Live | 0.5..1 | N | F/Home padding |
| Viewport/EditorCamera.hpp:126-127 | kMin/MaxDistance | 0.05 / 1e5 m | SETTING | editor.camera.{min,max}OrbitDistance | EditorCameraSettings | Editor Dev | Pref-P | Live | >0 | N | — |
| Viewport/EditorCamera.hpp:128 | kNearZ | 0.05 m | SETTING | editor.camera.nearClip | EditorCameraSettings | Editor | Pref-P | Live | 0.001..10 | N | UE exposes near clip |
| Viewport/EditorCamera.hpp:129 | kFarZ | 5000 m | SETTING | editor.camera.farClip | EditorCameraSettings | Editor | Pref-P | Live | 20..1e6 | N | large worlds; min 20, not 10 (S6-30): above nearClip's max of 10, so near < far and the frustum never degenerates |
| Viewport/EditorCamera.hpp:133 | kBaseFlySpeed | 5 m/s | SETTING | editor.camera.baseFlySpeed | EditorCameraSettings | Editor | Pref-P | Live | 0.1..100 | N | — |
| Viewport/EditorCamera.cpp:37 | DistanceScale ref / floor / cap | /10 m, 0.1, 1000 | SETTING | editor.camera.{distanceScaledSpeed,refDistance,floor,speedCap} | EditorCameraSettings | Editor Dev | Pref-P | Live | bool / >0 | N | UE has this as a toggle |
| Viewport/EditorCamera.cpp:72, :81 | look / orbit sensitivity | 0.2 deg/px | SETTING | editor.camera.{look,orbit}Sensitivity | EditorCameraSettings | Editor | Pref-P | Live | 0.01..2 | N | no invert-Y exists today either |
| Viewport/EditorCamera.cpp:73, :82 | pitch clamp | ±(90−1e-3) | CONSTANT | — | — | — | — | — | — | N | NaN guard (same as kMaxPitchDeg) |
| Viewport/EditorCamera.cpp:87 | fly boost | ×2 | SETTING | editor.camera.boostMultiplier | EditorCameraSettings | Editor | Pref-P | Live | 1..10 | N | — |
| Viewport/EditorCamera.cpp:114 | AdjustSpeed step + clamp | ×1.1/tick, 0.01..100 | SETTING | editor.camera.speedWheelStep | EditorCameraSettings | Editor Dev | Pref-P | Live | 1.01..2 | N | the clamp duplicates ViewportSettings |
| Viewport/EditorCamera.cpp:131 | framing radius floor | 0.05 m | CONSTANT | — | — | — | — | — | — | N | zero-extent guard; 0 is a bug |
| ArcaneClient/src/Arcane/Edit/Gizmo.hpp:95 | GizmoSnap::translate | 0.5 m | SETTING | editor.gizmo.snap.translate | EditorGizmoSnapSettings | Editor | Pref-P | Live | >0 | N | consumed by EditorAppFrame.cpp:1303 (reconciled R1) |
| ArcaneClient/src/Arcane/Edit/Gizmo.hpp:96 | GizmoSnap::rotationDeg | 15 deg | SETTING | editor.gizmo.snap.rotateDegrees | EditorGizmoSnapSettings | Editor | Pref-P | Live | 0.1..90 | N | (reconciled R1) |
| ArcaneClient/src/Arcane/Edit/Gizmo.hpp:97 | GizmoSnap::scale | 0.1 | SETTING | editor.gizmo.snap.scale | EditorGizmoSnapSettings | Editor | Pref-P | Live | >0 | N | (reconciled R1) |
| App/EditorApp.hpp:1165-1167 | gizmo mode / space / enabled defaults | Translate, World, false (Select tool) | SETTING | editor.gizmo.default{Mode,Space,Tool} | EditorGizmoSettings | Editor | Pref-P | NextWorld | enum | N | session-only today |
| App/EditorApp.hpp:1218 | m_physicsOverlay default | false | SETTING | editor.viewport.physicsOverlay | EditorViewportSettings | Editor Dev | Pref-P | Live | bool | N | session-only by ruling R5; keep session or persist |
| Panels/EditorPanels.cpp:1418 | Play frame thickness | 2 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.viewport.playFrameThickness) |
| Panels/EditorPanels.cpp:1460 | tool overlay inset | 8 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.viewport.overlayInset) |
| Viewport/ViewportGrid.hpp:62-63 | kGridFadeInPx / FullPx | 8 / 24 px | SETTING | editor.viewport.grid.fade{In,Full}Px | EditorGridSettings | Editor Dev | Pref-M | Live | >0 | N | not UI chrome: the fade band of the grid LOD, in screen px of line spacing (S5-2 review: restored from DERIVED) |
| Viewport/ViewportGrid.hpp:66-67 | kGridMinor/MajorAlpha | 0.35 / 0.55 | SETTING | editor.viewport.grid.{minor,major}Alpha | EditorGridSettings | Editor | Pref-M | Live | 0..1 | N | — |
| Viewport/ViewportGrid.hpp:75 | kGridLineThicknessPx | 1 px | SETTING | editor.viewport.grid.lineThickness | EditorGridSettings | Editor | Pref-M | Live | 0.5..4 | N | — |
| Viewport/ViewportGrid.cpp:20-21 | kMin/MaxDecade | 1e-9 .. 1e12 | CONSTANT | — | — | — | — | — | — | N | float-precision bounds of the decade LOD |
| Viewport/ViewportGrid.cpp:43 | kMaxLinesPerAxis | 16384 | SETTING | editor.viewport.grid.maxLinesPerAxis | EditorGridSettings | Editor Dev | Pref-M | Live | 256..65536 | N | draw budget |
| Viewport/ViewportGrid.cpp:109 / .hpp:87 | 3 simultaneous levels | 3 | CONSTANT | — | — | — | — | — | — | N | fixed array arity |
| Viewport/ViewportGrid.cpp:192, :200 | major every 10 | 10 | CONSTANT | — | — | — | — | — | — | N | decimal-decade identity |
| Viewport/DeferredPick.hpp:205 | kMaxFramesInFlight | 64 frames | SETTING | editor.viewport.pickMaxFramesInFlight | EditorViewportSettings | Editor Dev | Pref-M | Live | 4..1024 | N | abandon budget |
| App/EditorAppFrame.cpp:1391 | windowed Play simDt clamp | 0.25 s | DERIVED | (sim.maxFrameDeltaSeconds) | — | — | — | — | — | Y | spiral-of-death clamp; changes outcome after a stall (reconciled R1: read through sim.maxFrameDeltaSeconds, one clamp for the runtime and editor Play; was editor.play.maxSimDtSeconds) |
| App/EditorApp.cpp:2435 | first-frame viewport fallback width | 1280 px | SETTING | editor.viewport.fallbackExtentW | EditorViewportSettings | Editor Dev | Pref-M | Restart | 64..16384 | N | one frame only; a render-target extent, not UI chrome (S6-45 amendment): split from editor.viewport.fallbackExtent (S6-32) |
| App/EditorApp.cpp:2436 | first-frame viewport fallback height | 720 px | SETTING | editor.viewport.fallbackExtentH | EditorViewportSettings | Editor Dev | Pref-M | Restart | 64..16384 | N | as editor.viewport.fallbackExtentW (S6-45 amendment) |
| App/EditorAppFrame.cpp:538, :4034 | minimized / skipped-frame sleep | 1 ms | SETTING | app.window.minimizedSleepMs | AppWindowSettings | Game Dev | Pref-P | Live | 0..100 | N | no background or unfocused throttle exists (see Notes) (reconciled R1: the same minimized sleep as the runtime; the background throttle is the new editor.perf.backgroundFps) |
| — (new) | background / unfocused frame throttle | 0 (fps; 0 = no throttle) | SETTING | editor.perf.backgroundFps | EditorPerfSettings | Editor | Pref-M | Live | 0=off, 1..240 | N | new row (R1): 0 keeps today's behaviour (no throttle); kept apart from app.window.minimizedSleepMs on purpose (reconciled R1) |

### Graph/Node editor

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Widgets/GraphZoomLevels.hpp:60-65 | kZoomLevels | 20 stops, 0.1..2.0 | SETTING | editor.graph.zoomLevels | EditorGraphSettings | Editor Dev | Pref-M | Restart (canvas recreate) | sorted list | N | LOD tiers are defined against these stops |
| Widgets/GraphFit.cpp:44 | kNavigationZoomMargin | 0.1 | CONSTANT | — | — | — | — | — | — | N | mirrors the library's file-static value |
| Widgets/GraphNodeLod.hpp:53-56 | kLod{Lowest,Low,Medium,Default}Max | 0.200 / 0.250 / 0.675 / 1.375 | SETTING | editor.graph.lod.{lowestMax,lowMax,mediumMax,defaultMax} | GraphLodSettings | Editor Dev | Pref-M | Live | ascending, inside the zoom table | N | readability thresholds |
| Widgets/GraphNodeLod.hpp:79 | kEps | 1e-4 | CONSTANT | — | — | — | — | — | — | N | float-compare epsilon |
| Widgets/GraphGridPhase.hpp:82 | kZoomExponent | 0.7 | SETTING | editor.graph.grid.zoomExponent | GraphGridSettings | Editor Dev | Pref-M | Live | 0.1..1 | N | — |
| Widgets/GraphGridPhase.hpp:84 | kBaseSpacingPx | 20 | SETTING | editor.graph.grid.baseSpacing | GraphGridSettings | Editor Dev | Pref-M | Live | 4..128 | N | — |
| Widgets/GraphGridPhase.hpp:87 | kMinorTargetPx | 22 px | SETTING | editor.graph.grid.minorTargetPx | GraphGridSettings | Editor Dev | Pref-M | Live | 4..128 | N | kept SETTING: owned by S6-34; read as Ui::Px(setting) per S4-16 |
| Widgets/GraphGridPhase.hpp:91 | kMajorEvery | 8 | SETTING | editor.graph.grid.majorEvery | GraphGridSettings | Editor Dev | Pref-M | Live | {2,4,8,16} | N | must be a power of two |
| Widgets/GraphGridPhase.hpp:97 | kScaleEpsilon | 1e-4 | CONSTANT | — | — | — | — | — | — | N | divide-by-zero guard |
| Widgets/GraphCanvasStyle.hpp:42 | kGraphNodeRounding | 4 px | SETTING | editor.graph.nodeRounding | EditorGraphSettings | Editor | Pref-M | Restart | 0..16 | N | latched at CreateEditor |
| Widgets/GraphCanvasStyle.hpp:43-45 | node border widths (normal / hover / selected) | 1 / 1.5 / 2 px | SETTING | editor.graph.nodeBorder{,Hover,Selected}Width | EditorGraphSettings | Editor | Pref-M | Restart | 0..6 | N | — |
| Widgets/GraphCanvasStyle.hpp:51 | kGraphWireThickness | 2 px | SETTING | editor.graph.wireThickness | EditorGraphSettings | Editor | Pref-M | Live | 0.5..6 | N | — |
| Widgets/GraphCanvasStyle.hpp:56 | kGraphPinSegments | 12 | SETTING | editor.graph.pinSegments | EditorGraphSettings | Editor Dev | Pref-M | Live | 6..48 | N | tessellation |
| Widgets/GraphCanvasStyle.hpp:57, :62-63 | pin ring width, outer gap, outer width | 1.6 / 2.2 / 1.0 px | SETTING | editor.graph.pinRing.{width,outerGap,outerWidth} | GraphPinRingSettings | Editor Dev | Pref-M | Live | — | N | canvas-space geometry, scales with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Widgets/GraphWire.hpp:72 | kGraphLinkChannel | 7 | CONSTANT | — | — | — | — | — | — | N | the library's draw-channel index |
| Widgets/GraphWire.hpp:107 | brighten lerp | 0.25 | SETTING | editor.graph.wireHighlight | EditorGraphSettings | Editor Dev | Pref-M | Live | 0..1 | N | — |
| Widgets/GraphWire.hpp:196 | wire segments | 12..64, len/6 | SETTING | editor.graph.wireSegments{Min,Max,PxPer} | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | vertex budget |
| Widgets/GraphLegend.hpp:27-31 | legend inset / pads / gaps | 12, 10, 5, 14, 6 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.graph.legend.*) |
| Documents/ShaderGraphPinLegend.cpp:26 | kLegendDotRadius | 4 px | DERIVED | — | — | — | — | — | — | N | = kPinDotRadius (comment says it mirrors it) |
| Documents/ShaderGraphPinLegend.cpp:31-32 | legend pair / row gaps | 3 / 4 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.graph.legend.dotGap) |
| Documents/ShaderEditorDocument.cpp:391-392 | kNodePadX/Y | 10 / 6 px | SETTING | editor.graph.nodePadding | EditorGraphSettings | Editor Dev | Pref-M | Restart | 0..24 | N | canvas units at zoom 1, scales with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:404 | kNodeHeaderGap | 5 px | SETTING | editor.graph.nodeHeaderGap | EditorGraphSettings | Editor Dev | Pref-M | Live | 0..16 | N | kept SETTING: owned by S6-34; read as Ui::Px(setting) per S4-16 |
| Documents/ShaderEditorDocument.cpp:413 | kCullGuardBand | 0.25 | SETTING | editor.graph.cullGuardBand | EditorGraphSettings | Editor Dev | Pref-M | Live | 0..1 | N | UE GuardBandArea |
| Documents/ShaderEditorDocument.cpp:417 | kPinDotRadius | 4 px | SETTING | editor.graph.pinDotRadius | EditorGraphSettings | Editor | Pref-M | Live | 2..10 | N | — |
| Documents/ShaderEditorDocument.cpp:709 | kPinChipSlot | 2×(dot+gap+ring) | DERIVED | — | — | — | — | — | — | N | formula |
| Documents/ShaderEditorDocument.cpp:3363, :4829 | cfg.ShiftAddsToSelection | true | SETTING | editor.graph.shiftAddsToSelection | EditorGraphSettings | Editor | Pref-M | Restart | bool | N | UE modifier semantics |
| Documents/ShaderEditorDocument.cpp:1687 | checker kCell | 32 px @512 | SETTING | editor.shader.previewCheckerCell | ShaderEditorSettings | Editor Dev | Pref-M | Live | 4..128 | N | render-target texels (512 / 16 cells), not UI chrome and not scaled by editor.ui.scale (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:1689, :1697 | checker light colour / extent | (0.16,0.16,0.19), 0.8 | SETTING | editor.shader.{previewCheckerLight,previewCheckerSpriteScale} | ShaderEditorSettings | Editor Dev | Pref-M | Live | — | N | — |
| Documents/ShaderEditorDocument.cpp:4795 | kThumbMin | 96 px | SETTING | editor.graph.nodePreviewMinPx | EditorGraphSettings | Editor | Pref-M | Live | 32..512 | N | — |
| Documents/ShaderEditorDocument.hpp:985 | kNavHistoryMax | 32 | SETTING | editor.shader.navHistoryMax | ShaderEditorSettings | Editor Dev | Pref-M | Live | 1..256 | N | — |
| Documents/ShaderEditorDocument.hpp:1011 | kGraphPreviewSize | 512 px | SETTING | editor.shader.previewResolution | ShaderEditorSettings | Editor | Pref-M | Restart (reopen) | 128..2048 | N | GPU budget |
| Documents/ShaderEditorDocument.cpp:2094 | doc first-use size | 980×640 px | DERIVED | — | — | — | — | — | — | N | FirstUseEver; base px x editor.ui.scale (s16.11; was editor.documents.shaderInitialSize) |
| Documents/ShaderEditorDocument.cpp:3401-3416 | pass-chain auto-layout | 40, 190, 170, 90 px | SETTING | editor.shader.chainLayout.{originX,originY,pitchX,sceneOffsetX,sceneOffsetY} | ShaderChainLayoutSettings | Editor Dev | Pref-M | Live | — | N | canvas coordinates written into new .arcmat files: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:3999-4011 | new pass-graph node positions | (360,120), (100,120) | OTHER-STORE | (template data) | — | — | — | — | — | N | belongs in a shipped template asset, not a cvar (reconciled). belongs in a template asset (Notes) |
| App/EditorAppProject.cpp:1708-1716 | new-material template nodes + colour | (420,200), (160,200), (0.2,0.8,1,1) | OTHER-STORE | (template data) | — | — | — | — | — | N | belongs in a shipped template asset, not a cvar (reconciled). belongs in a template asset |
| Documents/ShaderEditorDocument.cpp:3530 | pass-name field | 120 px | SETTING | editor.graph.passNameFieldWidth | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | widgets inside canvas nodes scale with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:6060 | param-name field | 110 px | SETTING | editor.graph.paramNameFieldWidth | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | widgets inside canvas nodes scale with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:6195 | swizzle-mask field | 70 px | SETTING | editor.graph.swizzleFieldWidth | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | widgets inside canvas nodes scale with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:3583 | pass thumb | 72 px | SETTING | editor.shader.passThumbPx | ShaderEditorSettings | Editor Dev | Pref-M | Live | — | N | thumbnail inside a canvas node, scales with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:5921 | neutral-pin field, 1 lane | 64 px | SETTING | editor.graph.constPinNeutralWidth1 | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | widgets inside canvas nodes scale with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:5921 | neutral-pin field, 2 lanes | 106 px | SETTING | editor.graph.constPinNeutralWidth2 | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | widgets inside canvas nodes scale with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:5921 | neutral-pin field, 3-4 lanes | 190 px | SETTING | editor.graph.constPinNeutralWidth4 | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | widgets inside canvas nodes scale with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:5977, :6107 | Float / param default, 1 lane | 90 px | SETTING | editor.graph.constFloatWidth | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | widgets inside canvas nodes scale with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:5989, :6107 | Float2 / param default, 2 lanes | 140 px | SETTING | editor.graph.constFloat2Width | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | widgets inside canvas nodes scale with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:6002, :6107 | Float4 / Color / param default, 3-4 lanes | 220 px | SETTING | editor.graph.constFloat4Width | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | widgets inside canvas nodes scale with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:6133 | param range field | 120 px | SETTING | editor.graph.constParamRangeWidth | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | widgets inside canvas nodes scale with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Documents/ShaderEditorDocument.cpp:5930-5932, :5996, :6009, :6118-6120 | canvas drag speed | 0.01 /px | SETTING | editor.graph.dragSpeed | EditorGraphSettings | Editor | Pref-M | Live | 0.0001..1 | N | — |
| Documents/ShaderEditorDocument.cpp:6140 | range drag speed | 0.05 | SETTING | editor.graph.rangeDragSpeed | EditorGraphSettings | Editor Dev | Pref-M | Live | — | N | — |
| Documents/ShaderEditorDocument.cpp:5400, :5406 | rename dialog size / footer | 560×380, 34 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.shader.renameDialogSize) |
| Documents/ShaderEditorDocument.cpp:5445-5448 | rename targets listed | 8 | SETTING | editor.shader.renameListMax | ShaderEditorSettings | Editor Dev | Pref-M | Live | 1..64 | N | — |
| Documents/ShaderEditorDocument.cpp:6235-6250 | custom-node body preview | 8 lines × 48 chars | SETTING | editor.shader.bodyPreview{Lines,Chars} | ShaderEditorSettings | Editor Dev | Pref-M | Live | — | N | — |
| Documents/ShaderEditorDocument.cpp:6816, :6905, :6908, :2696, :2748 | page drag speed | 0.01 | DERIVED | — | — | — | — | — | — | N | = PropertyGrid default speed |
| Panels/AssetGraphPanel.cpp:108 | kGraphFocusComboWidth | 280 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assetGraph.focusComboWidth) |
| Panels/AssetGraphPanel.cpp:109 | kGraphFocusHitCap | 12 | SETTING | editor.assetGraph.focusHitCap | AssetGraphSettings | Editor | Pref-M | Live | 1..100 | N | — |
| Panels/AssetGraphPanel.cpp:358-361 | node min/max width, header height, accent bar | 180/220/24/3 px | SETTING | editor.assetGraph.node.{minWidth,maxWidth,headerHeight,accentBarWidth} | AssetGraphNodeSettings | Editor Dev | Pref-M | Live | — | N | canvas-space node geometry, scales with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Panels/AssetGraphPanel.cpp:365 | kGraphPinRadius | 4.5 px | SETTING | editor.assetGraph.pinRadius | AssetGraphSettings | Editor Dev | Pref-M | Live | — | N | canvas-space pin geometry, scales with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Panels/AssetGraphPanel.cpp:388-389 | column / row pitch | 300 / 90 px | SETTING | editor.assetGraph.layout{Column,Row}Pitch | AssetGraphSettings | Editor | Pref-M | Live | 100..1000 | N | layout density |
| Panels/AssetGraphPanel.cpp:395-398 | node pads / icon gap | 3+8, 8, 6, 6 px | SETTING | editor.assetGraph.node.padding | AssetGraphNodeSettings | Editor Dev | Pref-M | Live | — | N | :395 is DERIVED from the accent bar; canvas-space node geometry, scales with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Panels/AssetGraphPanel.cpp:479 | overflow wire thickness | 1.5 px | SETTING | editor.assetGraph.overflowWireThickness | AssetGraphSettings | Editor Dev | Pref-M | Live | — | N | canvas-space wire, scales with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Panels/AssetGraphPanel.cpp:484-485, :489 | wire dims, ghost wash | 0.62 / 0.78 / 0.55 | SETTING | editor.assetGraph.{wireDim,overflowDim,ghostWash} | AssetGraphSettings | Editor Dev | Pref-M | Live | 0..1 | N | — |
| Panels/AssetGraphPanel.cpp:496-497, :502 | dash on/off, end dot | 6 / 5 / 4 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assetGraph.dash.*) |
| Panels/AssetGraphPanel.cpp:510 | kGraphDashMaxCells | 256 | SETTING | editor.assetGraph.dashMaxCells | AssetGraphSettings | Editor Dev | Pref-M | Live | 16..4096 | N | budget |
| Panels/AssetGraphPanel.cpp:1166-1167 | legend swatch | 18×2 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assetGraph.legendSwatch) |
| Panels/AssetGraphPanel.cpp:1807 | label pill pad | 3 px | SETTING | editor.assetGraph.labelPad | AssetGraphSettings | Editor Dev | Pref-M | Live | — | N | canvas-space label pill, scales with the graph zoom: not UI chrome (S5-2 review: restored from DERIVED) |
| Panels/AssetGraphPanel.cpp:2592 | selection-strip thumb | 36 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assetGraph.stripThumbPx) |
| Panels/AssetGraphPanel.hpp:44 | kAssetGraphSelectionStripH | 48 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assetGraph.stripHeight) |
| Panels/AssetGraphViewModel.hpp:115 | depthLimit | 2 | SETTING | editor.assetGraph.defaultDepth | AssetGraphSettings | Editor | Pref-P | Live | 1..16 | N | — |
| Panels/AssetGraphViewModel.hpp:116 | breadthCap | 20 | SETTING | editor.assetGraph.breadthCap | AssetGraphSettings | Editor | Pref-P | Live | 1..500 | N | — |
| Panels/AssetGraphPanel.cpp:2394 | hover delay | style.HoverStationaryDelay | DERIVED | — | — | — | — | — | — | N | ImGui style value |

### Inspector/PropertyGrid

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Panels/EditorPanels.cpp:2526 | kInspectorFramePaddingY | 3 px | SETTING | editor.inspector.framePaddingY | InspectorSettings | Editor | Pref-M | Live | 0..12 | N | row rhythm |
| Panels/EditorPanels.cpp:2527 | kInspectorItemSpacingY | 4 px | SETTING | editor.inspector.itemSpacingY | InspectorSettings | Editor | Pref-M | Live | 0..12 | N | — |
| Widgets/EditorWidgets.cpp:85 | kLabelColumnFraction | 0.4 | SETTING | editor.inspector.labelColumnFraction | InspectorSettings | Editor | Pref-M | Live | 0.2..0.7 | N | UE exposes the splitter |
| Widgets/EditorWidgets.cpp:95 | kLabelSeedMinAvailEm | 8 em | SETTING | editor.inspector.labelSeedMinEm | InspectorSettings | Editor Dev | Pref-M | Live | — | N | — |
| Widgets/EditorWidgets.cpp:252 | kAxisBarWidth | 3 px | DERIVED | — | — | — | — | — | — | N | matches ImGuiStyle::ColorMarkerSize |
| Widgets/EditorWidgets.hpp:256 | kAssetRowThumbSize | 18 px | SETTING | editor.ui.assetRowThumbPx | EditorUiSettings | Editor Dev | Pref-M | Live | — | N | kept SETTING: owned by S6-28; read as Ui::Px(setting) per S4-16 |
| Panels/AssetInspectorSource.cpp:37 | kAssetPageThumbSize | 140 px | SETTING | editor.inspector.assetThumbMaxPx | InspectorSettings | Editor | Pref-M | Live | 64..512 | N | it is also assetThumbMinPx's max (:40), so DERIVED there |
| Panels/AssetInspectorSource.cpp:68-69 | compact header min / text column min | 250 / 110 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.inspector.previewCompact*) |
| Panels/AssetInspectorSource.cpp:50, :56 | fallback 64 / 0.30 | dup of cvar defaults | DERIVED | — | — | — | — | — | — | N | shadow copy; delete (s10.2) |
| Documents/ShaderEditorDocument.cpp:94, :704 | fallback 0.45 / 16 | dup | DERIVED | — | — | — | — | — | — | N | shadow copies |
| Panels/InspectorHost.hpp:50 | kHistoryDepth | 32 | SETTING | editor.inspector.historyDepth | InspectorSettings | Editor | Pref-M | Live | 1..256 | N | — |
| Panels/InspectorHost.hpp:54 | kMaxInstances | 8 | SETTING | editor.inspector.maxInstances | InspectorSettings | Editor Dev | Pref-M | Restart | 2..32 | N | lowering it drops persisted Ids= |
| Panels/InspectorHost.hpp:56 | kAssetsInstanceId | 1 | CONSTANT | — | — | — | — | — | — | N | window-id contract ("###inspector_1") |
| Panels/InspectorWindows.hpp:45 | kInspectorHeaderMinCrumbWidth | 120 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.inspector.minCrumbWidth) |
| Panels/InspectorWindows.cpp:118 | header width formula | FramePadding.x×4 + spacing | DERIVED | — | — | — | — | — | — | N | style-relative |
| Panels/InspectorFields.cpp:390 | kTolerance | 1e-5 | CONSTANT | — | — | — | — | — | — | N | float-equality epsilon for mixed values |
| Panels/InspectorView.cpp:714, :742, :767, :852 | drag speed | 0.1 /px | SETTING | editor.inspector.dragSpeed | InspectorSettings | Editor | Pref-M | Live | 0.001..10 | N | — |
| Panels/InspectorView.cpp:956-957 | rotation drag speed | 0.5 deg/px (0.01 rad) | SETTING | editor.inspector.rotationDragSpeedDeg | InspectorSettings | Editor | Pref-M | Live | 0.01..10 | N | — |
| Widgets/PropertyGrid.hpp:205, :212 | FloatRow / VecRow default speed | 0.01 | SETTING | editor.ui.propertyDragSpeed | EditorUiSettings | Editor | Pref-M | Live | 0.0001..1 | N | (reconciled R1: one struct per prefix, editor.ui.* -> EditorUiSettings) |
| Widgets/PropertyGrid.cpp:358 | IntRow step / stepFast | 1 / 100 | SETTING | editor.ui.intStep{,Fast} | EditorUiSettings | Editor Dev | Pref-M | Live | — | N | (reconciled R1: one struct per prefix, editor.ui.* -> EditorUiSettings) |
| Widgets/PropertyGrid.cpp:96 | refused-draft hold | 3 frames | CONSTANT | — | — | — | — | — | — | N | frame-protocol timing; another value is a focus bug |
| Panels/EditorPanels.cpp:1783, :1796 | Add Component popup | 260 px, 260×260 | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.inspector.addComponentPopupSize) |
| Documents/SpriteDocument.cpp:381 | PPU drag speed | 0.5 | SETTING | editor.sprite.ppuDragSpeed | SpriteDocSettings | Editor Dev | Pref-M | Live | 0.001..100 | N | authoring (S6-45 amendment): split from editor.sprite.ppuRange (S6-35), one name per value |
| Documents/SpriteDocument.cpp:381 | PPU minimum | 1 | SETTING | editor.sprite.ppuMin | SpriteDocSettings | Editor Dev | Pref-M | Live | 0.001..65536 | N | authoring range (S6-45 amendment) |
| Documents/SpriteDocument.cpp:381 | PPU maximum | 4096 | SETTING | editor.sprite.ppuMax | SpriteDocSettings | Editor Dev | Pref-M | Live | 1..65536 | N | authoring range (S6-45 amendment) (S6-42 widens it only to a sprite's current value) |
| Documents/SpriteDocument.cpp:415 | pivot drag speed | 0.005 | SETTING | editor.sprite.pivotDragSpeed | SpriteDocSettings | Editor Dev | Pref-M | Live | — | N | — |
| Documents/MeshDocument.cpp:699-716 | primitive ranges | subdiv 1..64, rings 2/3..64/128, seg 3..128, ratio 1..20 @0.02 | SETTING | editor.mesh.primitiveRanges.{planeSubdivisionsMin,planeSubdivisionsMax,sphereRingsMin,sphereRingsMax,segmentsMin,segmentsMax,capsuleRingsMin,capsuleRingsMax,capsuleLengthRatioMin,capsuleLengthRatioMax} | EditorMeshPrimitiveRangesSettings | Editor Dev | Pref-M | Live | — | N | authoring caps |
| Documents/MeshDocument.cpp:723 | capsule length ratio drag speed | 0.02 | SETTING | editor.mesh.capsuleRatioDragSpeed | MeshDocSettings | Editor Dev | Pref-M | Live | 0.0001..1 | N | (S6-45 amendment): the "@0.02" of the primitive-ranges row, dropped from that family as a drag speed; S6-35 registered it |
| Panels/TextureImportSettings.cpp:74 | Max Size range | 0..16384 | CONSTANT | — | — | — | — | — | — | N | D3D12 max texture dimension (hardware) |

### Asset Browser

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Panels/AssetBrowserPanel.cpp:57 | kRailWidth | 180 px | SETTING | editor.assets.railWidth | EditorAssetsSettings | Editor | Pref-M | Live | 80..600 | N | — |
| Panels/AssetBrowserPanel.cpp:58 | kRailRowHeight | 26 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assets.railRowHeight) |
| Panels/AssetBrowserPanel.cpp:59, :65 | kChildIndent / kGroupIndent | 20 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assets.indent) |
| Panels/AssetBrowserPanel.cpp:466 | kGroupCountGap | 6 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assets.groupCountGap) |
| Panels/AssetBrowserPanel.cpp:709 | kBadgeMargin | 3 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assets.badgeMargin) |
| Panels/AssetBrowserPanel.cpp:560, :1154 | rename / search minimum widths | 60 / 80 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assets.minFieldWidth) |
| Panels/AssetPanelCommon.hpp:395 | kAssetPanelToolbarFramePadY | 4 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assets.toolbarPadY) |
| Panels/AssetPanelCommon.hpp:396 | kAssetPanelBottomBarHeight | 24 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assets.bottomBarHeight) |
| Panels/AssetPanelCommon.hpp:406 | kAssetPanelToolbarBodyGapPx | 7 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assets.toolbarGap) |
| Panels/AssetPanelCommon.hpp:414 | kTableRowHeight | 24 px | SETTING | editor.ui.tableRowHeight | EditorUiSettings | Editor | Pref-M | Live | 16..48 | N | density (reconciled R1: one struct per prefix, editor.ui.* -> EditorUiSettings) |
| Panels/AssetPanelCommon.cpp:415-416 | tooltip width / thumb | 210 / 64 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assets.tooltip{Width,Thumb}) |
| Panels/AssetPanelCommon.cpp:452 | kNamedTargets | 3 | SETTING | editor.assets.namedTargets | EditorAssetsSettings | Editor Dev | Pref-M | Live | 1..20 | N | — |
| Panels/AssetReferenceField.cpp:22 | kAssetRefThumbSize | 20 px | SETTING | editor.ui.assetRefThumbPx | EditorUiSettings | Editor Dev | Pref-M | Live | — | N | its comment says "not a tunable"; kept SETTING: owned by S6-28; read as Ui::Px(setting) per S4-16 (S5-2 review settled) |
| Panels/AssetReferenceField.cpp:90, :234 | row offset / min name width | 24 / 16 px | DERIVED | — | — | — | — | — | — | N | = row height / a floor |
| Panels/AssetStatusPanel.cpp:67-71 | tile height / min width, section gap, progress height, selection border | 64/72/6/4/2 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assetStatus.*) |
| Panels/AssetStatusPanel.cpp:88-89 | right column width, caption gap | 300 / 2 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assetStatus.rightColumnWidth) |
| Panels/AssetStatusPanel.cpp:707 | right column max share | 0.45 | SETTING | editor.assetStatus.rightColumnMaxFraction | AssetStatusSettings | Editor Dev | Pref-M | Live | 0.2..0.8 | N | — |
| Panels/AssetStatusPanel.cpp:367-376 | "ago" buckets | 60 s / 60 min | CONSTANT | — | — | — | — | — | — | N | calendar arithmetic |
| Panels/AssetActivityLog.hpp:69 | kCapacity | 100 entries | SETTING | editor.assets.activityLogCapacity | EditorAssetsSettings | Editor | Pref-M | Restart | 10..10000 | N | — |
| Panels/CreateAssetDialog.cpp:31, :35, :39, :41 | dialog width, picker row height, visible rows, footer button | 380 px, 24 px, 6, 92 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assets.createDialog.*) |
| Panels/CreateAssetDialog.hpp:276 | kMaterialSurfaceDefaultIndex | 2 (post) | SETTING | editor.assets.newMaterialDefaultSurface | EditorAssetsSettings | Editor | Pref-P | Live | {sprite, mesh, post} | N | a reasonable default to change |
| Panels/CreateAssetDialog.hpp:332 | kCreateNameMaxPathChars | 240 | CONSTANT | — | — | — | — | — | — | N | Windows MAX_PATH margin |
| Panels/CreateAssetDialog.hpp:351 | kDenied | `\/:*?"<>\|` | CONSTANT | — | — | — | — | — | — | N | OS filename rules |
| Panels/AssetFileOpDialogs.cpp:17, :37 | modal widths | 380 / 440 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.assets.fileOpDialogWidth) |
| Panels/AssetFileOpDialogs.cpp:25-103 | button widths | 92 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.ui.dialogButtonWidth) |
| Project/AssetFileOps.cpp:266 | unique-name probe cap | 100000 | CONSTANT | — | — | — | — | — | — | N | loop safety bound |
| App/EditorAppProject.cpp:430 | asset-watch poll | 1.0 s | SETTING | editor.assets.watchPollSeconds | EditorAssetsSettings | Editor | Pref-M | Live | 0.1..30 | N | hot-reload latency vs I/O |
| App/EditorAppProject.cpp:481 | content-discovery poll | 2.0 s | SETTING | editor.assets.discoveryPollSeconds | EditorAssetsSettings | Editor | Pref-M | Live | 0.5..60 | N | — |
| App/EditorApp.cpp:689 | shader-compile debounce | 0.2 s | SETTING | render.shader.compileDebounceSeconds | RenderShaderSettings | Game Dev | Pref-P | Restart | 0..2 | N | passed to Initialize once (reconciled R1; range reconciled with RuntimeApp.cpp:311 at the S5-2 review) |
| Project/MaterialPreviewHarvester.cpp:44 | kThumbSize | 64 px | SETTING | editor.thumbnail.size | EditorThumbnailSettings | Editor Dev | Project | Restart | 32..256 | N | also the on-disk PNG size; a change invalidates Saved/Thumbnails (reconciled R1: Project scope, thumbnails are a shared cache and goldens depend on them) |
| Project/MaterialPreviewHarvester.cpp:48 | kCheckerCell | 16 px | DERIVED | — | — | — | — | — | — | N | 32@512 scaled to 64 |
| Project/MaterialPreviewHarvester.cpp:55 | kThumbTime | 0.35 s | SETTING | editor.thumbnail.time | EditorThumbnailSettings | Editor Dev | Project | Restart | 0..10 | N | a change invalidates cached PNGs (reconciled R1: Project scope, thumbnails are a shared cache and goldens depend on them) |
| Project/MaterialPreviewHarvester.cpp:393 | kMaxVehicleDrops | 3 | SETTING | editor.thumbnail.maxRetries | EditorThumbnailSettings | Editor Dev | Project | Restart | 1..20 | N | (reconciled R1: Project scope, thumbnails are a shared cache and goldens depend on them) |
| Project/MaterialPreviewHarvester.cpp:1237 | kMeshThumbFovDegrees | 35 deg | SETTING | editor.thumbnail.meshFovDegrees | EditorThumbnailSettings | Editor Dev | Project | Restart | 10..90 | N | thumbnail framing (reconciled R1: Project scope, thumbnails are a shared cache and goldens depend on them) |
| Project/MaterialPreviewHarvester.cpp:1176, :1186 | checker light, extent | (0.16,0.16,0.19), 0.8 | DERIVED | — | — | — | — | — | — | N | duplicates ShaderEditorDocument.cpp:1689/1697 |
| Project/MaterialPreviewHarvester.cpp:1257-1259 | light dir / ambient | (0.45,0.7,0.8), 0.12 | DERIVED | — | — | — | — | — | — | N | = editor.preview.light* (one shared value) |
| Project/MeshImportWave.cpp:361 | kMinRadius | 0.5 m | DERIVED | — | — | — | — | — | — | N | mirrors the BuildUvSphere(0.5) guard |
| Project/MeshImportWave.cpp:366 | kMargin | 0.15 | SETTING | editor.thumbnail.framingMargin | EditorThumbnailSettings | Editor Dev | Project | Restart | 0..1 | N | (reconciled R1: Project scope, thumbnails are a shared cache and goldens depend on them) |
| Project/MeshImportWave.cpp:385-386 | near/far fit | radius×1.5 | DERIVED | — | — | — | — | — | — | N | framing formula |
| App/EditorApp.cpp:1962 | Hub cover width | 512 px | CONSTANT | — | — | — | — | — | — | N | the Hub cover contract (shared WriteThumbnailPngRgba; Hub caps 2 MiB) |
| App/EditorApp.cpp:2346 | toolbar logo texture | 64 px | DERIVED | — | — | — | — | — | — | N | 2× the on-screen mark |

### Documents

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Documents/InputActionsDocument.cpp:145, :171 | rebind capture timeout | 10 s | SETTING | editor.input.rebindTimeoutSeconds | InputEditorSettings | Editor | Pref-M | Live | 1..60 | N | — |
| Documents/InputActionsDocument.cpp:231 | DeltaTime fallback | 1/60 s | CONSTANT | — | — | — | — | — | — | N | zero-dt guard |
| Documents/InputActionsDocumentWidgets.hpp:83 | preview evaluator tick | 1/60 s | DERIVED | — | — | — | — | — | — | N | should be the project's fixed step |
| Documents/InputActionsDocumentWidgets.cpp:19 | kMapsColumnWidth | 180 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.input.mapsColumnWidth) |
| Documents/InputActionsDocumentWidgets.cpp:20 | kIndent | 16 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.input.indent) |
| Documents/InputActionsDocumentWidgets.cpp:147, :149, :575, :640 | row height 24 | 24 px | DERIVED | — | — | — | — | — | — | N | = kTableRowHeight, repeated |
| Documents/InputActionsDocumentWidgets.cpp:249, :252, :662, :664 | field widths | 200/160/120/120 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.input.fieldWidths) |
| Documents/InputActionsDocumentWidgets.cpp:475 | live-value highlight base | 0.12 alpha | SETTING | editor.input.liveHighlightBase | InputEditorSettings | Editor Dev | Pref-M | Live | 0..1 | N | (S6-45 amendment): split from editor.input.liveHighlight (base + gain x v), one name per value |
| Documents/InputActionsDocumentWidgets.cpp:475 | live-value highlight gain | 0.2 alpha | SETTING | editor.input.liveHighlightGain | InputEditorSettings | Editor Dev | Pref-M | Live | 0..1 | N | as editor.input.liveHighlightBase (S6-45 amendment) |
| Documents/InputActionsInspectorPage.cpp:257, :360 | hold-seconds / scale drag speed | 0.01 | DERIVED | — | — | — | — | — | — | N | = PropertyGrid speed |
| Documents/InputActionsInspectorPage.cpp:392, :420, :433 | picker widths | 260, 300×260, 200 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.input.pickerSize) |
| Documents/CrashReportDocument.cpp:79 | log tail | 200 lines | SETTING | diagnostics.logTailLines | DiagnosticsSettings | Game Dev | Pref-P | Live | 0..10000 | N | (reconciled R1) (S6-45 amendment): one range for the one value, the reporter row's 0..10000 the code declares (was 20..5000 here) |
| Documents/CrashReportDocument.cpp:168 | window first size | 760×760 px | SETTING | editor.crash.initialSize | CrashViewerSettings | Editor | Pref-M | Live | — | N | the crash document's first-use window size, a window dimension and not UI chrome (S5-2 review: restored from DERIVED; owned by S6-40) |
| Documents/CrashReportDocument.cpp:202; Panels/EditorPanels.cpp:1217 | "Copied" flash | 0.75 s | SETTING | ui.copyFlashSeconds | UiSettings | Game Dev | Pref-M | Live | 0..5 | N | two copies (reconciled R1) |
| Documents/CrashReportDocument.cpp:262 | field width | 320 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.crash.*; the text-row count is split out below) |
| Documents/CrashReportDocument.cpp:387 | text rows | 16 lines | SETTING | editor.crash.textRows | CrashViewerSettings | Editor | Pref-M | Live | 4..200 | N | a row count, not a px value (S5-2 review: split from the :262 px row; owned by S6-40, which declares it Editor / Pref-M / Live beside editor.crash.maxRows) |
| Settings/SettingsApply.cpp:9 | archive save debounce | 500 ms | SETTING | editor.settings.saveDebounceMs | SettingsWindowCVars | Editor | Pref-M | Live | 0..10000 | N | (S6-45 amendment): how long the settings windows wait after an edit before writing the archive (S3) |
| Settings/SettingsHost.cpp:18 | open at boot | "" | SETTING | editor.settings.openAtBoot | SettingsWindowCVars | Editor Dev | Pref-M | Live | string | N | (S6-45 amendment): Hidden; automation opens a settings window at boot (S3) |
| Settings/SettingsHost.cpp:23 | open category | "" | SETTING | editor.settings.openCategory | SettingsWindowCVars | Editor Dev | Pref-M | Live | string | N | (S6-45 amendment): Hidden; the category the window opens on (S3) |
| Settings/ShortcutsPage.cpp:20 | conflicts-only filter | false | SETTING | editor.settings.keysConflictsOnly | SettingsWindowCVars | Editor Dev | Pref-M | Live | bool | N | (S6-45 amendment): Hidden; the Shortcuts page's filter state (S4-14) |
| Documents/CrashReportDocument.cpp:283 | max rows | 24 | SETTING | editor.crash.maxRows | CrashViewerSettings | Editor | Pref-M | Live | — | N | a row count, not a px value (split from the :262/:387 row; owned by S6-40, which declares it Editor / Pref-M / Live) |
| Documents/MeshDocument.hpp:448 | kPreviewSize | 512 px | SETTING | editor.mesh.previewResolution | MeshDocSettings | Editor | Pref-M | Restart (reopen) | 128..2048 | N | GPU budget |
| Documents/MeshDocument.cpp:395-396 | kFovYDegrees / kMargin | 45 deg / 1.5 | SETTING | editor.mesh.preview{Fov,Margin} | MeshDocSettings | Editor Dev | Pref-M | Live | — | N | — |
| Documents/MeshDocument.cpp:402, :422-423, :431 | view dir, near/far, light dir, ambient | (1,0.75,1), 0.05/4d+1, (0.4,1,0.3), 0.12 | SETTING | editor.preview.light.direction / .ambient | EditorPreviewLightSettings | Editor | Pref-M | Live | — | N | a different light dir from the material previews (inconsistent) (reconciled R1: the default moves to the shared value (0.45,0.7,0.8); no golden captures the mesh document) |
| Documents/MeshDocument.cpp:516, :549 | window size, info child | 420×640, 220 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.mesh.initialSize) |
| Documents/MaterialSpherePreview.hpp:34 | sphere rings | 24 | SETTING | editor.preview.sphereRings | PreviewSettings | Editor Dev | Pref-M | Restart | 3..128 | N | (S6-45 amendment): split from the 24 x 32 row, one name per value (S6-35 registered both) |
| Documents/MaterialSpherePreview.hpp:34 | sphere segments | 32 | SETTING | editor.preview.sphereSegments | PreviewSettings | Editor Dev | Pref-M | Restart | 3..128 | N | — |
| Documents/MaterialSpherePreview.hpp:57 | preview projection | 35 deg, 0.05..10 | SETTING | editor.preview.sphereFov | PreviewSettings | Editor Dev | Pref-M | Live | — | N | — |
| Documents/MaterialSpherePreview.hpp:58, :60 | light dir / ambient | (0.45,0.7,0.8), 0.12 | SETTING | editor.preview.light.direction / .ambient / .colour | EditorPreviewLightSettings | Editor | Pref-M | Live | — | N | canonical copy (reconciled R1: one shared preview light, colour (1,1,1)) |
| Documents/SpriteDocument.cpp:190, :250 | window size, min preview side | 420×560, 16 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.sprite.initialSize) |
| App/EditorApp.cpp:1272 | m_scriptedOpenFocusFrames | 3 | CONSTANT | — | — | — | — | — | — | N | automation-only frame protocol |
| App/EditorAppFrame.cpp:2922, :2926, :2932 | start page column / spacers | 640, 24, 16 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.startPage.columnWidth) |

### Undo

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| App/UndoSettings.cpp:13 | editor.undo.maxSteps | 100 steps | SETTING (exists) | editor.undo.maxSteps | EditorUndoSettings | Editor | Pref-P | Live (pushed into the stack) | 1..10000 | N | already a cvar |
| App/UndoSettings.cpp:16 | editor.undo.byteBudgetMB | 512 MB | SETTING (exists) | editor.undo.byteBudgetMB | EditorUndoSettings | Editor | Pref-P | Live | 16..65536 | N | already a cvar |
| App/UndoSettings.cpp:19 | editor.undo.spillThresholdKB | 256 KB | SETTING (exists) | editor.undo.spillThresholdKB | EditorUndoSettings | Editor | Pref-P | Live | 16..1048576 | N | already a cvar |
| App/UndoSettings.cpp:33-35 | ReadInt fallbacks | 100 / 512 / 256 | DERIVED | — | — | — | — | — | — | N | shadow copies of the defaults; delete (s10.2) |
| Scene/SceneSession.hpp:76 | kUnreachableStateId | ~0ull | CONSTANT | — | — | — | — | — | — | N | sentinel |

### Console/Problems

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| App/EditorApp.hpp:848 | ConsoleBuffer capacity | 512 lines | SETTING | editor.console.ringLines | EditorConsoleSettings | Editor | Pref-M | Restart | 64..100000 | N | ring size (reconciled R1: the editor Panels/ConsoleModel is already a 512-line ring) |
| Panels/EditorPanels.hpp:281 | lineCap | 512 | SETTING | editor.console.displayLineCap | EditorConsoleSettings | Editor | Pref-M | Live | 64..100000 | N | — |
| Panels/EditorPanels.hpp:276-278 | collapse / autoScroll / wrap defaults | false / true / true | SETTING | editor.console.{collapse,autoScroll,wrap} | EditorConsoleSettings | Editor | Pref-M | Live | bool | N | session-only today |
| Panels/EditorPanels.cpp:1155 | cvar reply lines reserved | 6 | SETTING | editor.console.replyLines | EditorConsoleSettings | Editor Dev | Pref-M | Live | 0..32 | N | — |
| Panels/EditorPanels.cpp:1123 | category combo width | 140 px | DERIVED | — | — | — | — | — | — | N | base px x editor.ui.scale (s16.11; was editor.console.categoryComboWidth) |
| ArcaneCore/src/Arcane/Config/CVarRegistry.cpp:123 | console.historySize | 64 | SETTING (exists) | console.historySize | ConsoleSettings | Game | Pref-P | Live | 1..1024 | N | registered with module "engine" (reconciled R1: Game, Pref-P; it serves both consoles) |
| ArcaneCore/src/Arcane/Base/Log.cpp:258-263 | log.level | Init arg | SETTING (exists) | log.level | LogSettings | Game | Pref-P | Live | 0..6 | N | already Archive and Dev (reconciled R1) |
| Panels/ConsoleModel.cpp:18 | kPrefixRules | table | CONSTANT | — | — | — | — | — | — | N | log-format mapping |
| Panels/ConsoleModel.cpp:48, :52 | prefix length ≤24 | 24 chars | CONSTANT | — | — | — | — | — | — | N | log-format parse rule |
| Panels/ConsoleModel.cpp:114 | category column pad | 8 chars | SETTING | editor.console.categoryWidth | EditorConsoleSettings | Editor Dev | Pref-M | Live | 0..32 | N | display width |
| Panels/DiagnosticStore.hpp:23 | SeverityMask | bit values | CONSTANT | — | — | — | — | — | — | N | bitmask encoding |

### Hub/Recents

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Project/RecentProjects.hpp:70 | Recents::kMaxShown | 10 | SETTING | editor.recents.maxProjectsShown | RecentsSettings | Editor | Pref-M | Live | 1..50 | N | — |
| Project/SceneRecents.hpp:27 | kMaxEntries | 10 | SETTING | editor.recents.maxScenes | RecentsSettings | Editor | Pref-M | Live | 1..50 | N | — |
| Project/RecentProjects.cpp:30, :32 | kFormatVersion 1, ".arcproj" | — | CONSTANT | — | — | — | — | — | — | N | Hub file format |
| Project/SceneRecents.hpp:32 | kFormatVersion | 1 | CONSTANT | — | — | — | — | — | — | N | file format |
| Project/StartPageModel.cpp:11-18 | relative-time buckets | 60 s / 1 h / 1 d / 30 d | CONSTANT | — | — | — | — | — | — | N | calendar arithmetic |
| ArcaneHub/src-tauri/src/settings.rs:78 | default_project_dir | "" | OTHER-STORE | (Hub settings.archub) | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). the Hub is Rust; it cannot join the cvar registry |
| ArcaneHub/src-tauri/src/settings.rs:97 | launch_behavior | "tray" | OTHER-STORE | (Hub) | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). — |
| ArcaneHub/src-tauri/src/settings.rs:109 | project_view | "grid" | OTHER-STORE | (Hub) | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). — |
| ArcaneHub/src-tauri/src/settings.rs:117, :123 | project_sort / sort_desc | "opened" / true | OTHER-STORE | (Hub) | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). — |
| ArcaneHub/src-tauri/src/settings.rs:135 | confirm_delete | true | OTHER-STORE | (Hub) | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). — |
| ArcaneHub/src-tauri/src/launch.rs:67 | BOOT_WATCHDOG | 2 s | OTHER-STORE | (Hub) hub.bootWatchdogSeconds | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). slow disks |
| ArcaneHub/src-tauri/src/spawn.rs:93-94 | PROBE_TIMEOUT / PROBE_POLL | 10 s / 25 ms | OTHER-STORE | (Hub) | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). — |
| ArcaneHub/src-tauri/src/tray.rs:25 | QUICK_LAUNCH | 5 | OTHER-STORE | (Hub) | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). — |
| ArcaneHub/src-tauri/src/watch.rs:24 | disk watch poll | 2 s | OTHER-STORE | (Hub) | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). — |
| ArcaneHub/src-tauri/src/resolve.rs:109 | COVER_MAX_BYTES | 2 MiB | CONSTANT | — | — | — | — | — | — | N | cover contract with the editor's 512 px PNG |
| ArcaneHub/src-tauri/src/resolve.rs:150 | SCAN_VISIT_BUDGET | 100000 | OTHER-STORE | (Hub) | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). budget |
| ArcaneHub/src-tauri/src/project.rs:198 | MAX_NAME_LEN | 64 | OTHER-STORE | (Hub) | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). check parity with engine Project validation (not verified) |
| ArcaneHub/src-tauri/src/store.rs:34; project.rs:13 | format versions | 1 | CONSTANT | — | — | — | — | — | — | N | file format |
| ArcaneHub/src/lib/theme.css:45-99 | Hub theme tokens (25 vars: surfaces, text, accent #A24349, radii, durations) | — | OTHER-STORE | (Hub-local) | — | — | — | — | — | N | Hub-owned store (Rust/Tauri, separate process); JSON bridge later (reconciled). a separate palette from EditorTheme; unify or keep |

### Layout

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Panels/DefaultLayout.hpp:41 | kDefaultInspectorWidthPx | 380 px (layout) | SETTING | editor.layout.factory.inspectorWidth | LayoutFactorySettings | Editor Dev | Pref-M | Live (on Reset Layout) | — | N | factory layout (s16.11: a factory layout size, not px-metric chrome) |
| Panels/DefaultLayout.hpp:42 | kDefaultOutlinerWidthPx | 270 px (layout) | SETTING | editor.layout.factory.outlinerWidth | LayoutFactorySettings | Editor Dev | Pref-M | Live | — | N | (s16.11: a factory layout size, not px-metric chrome) |
| Panels/DefaultLayout.hpp:43 | kDefaultBottomBandPx | 350 px (layout) | SETTING | editor.layout.factory.bottomBand | LayoutFactorySettings | Editor Dev | Pref-M | Live | — | N | (s16.11: a factory layout size, not px-metric chrome) |
| Panels/DefaultLayout.hpp:44 | kDefaultCentralMinFraction | 0.40 | SETTING | editor.layout.factory.centralMinFraction | LayoutFactorySettings | Editor Dev | Pref-M | Live | 0.1..0.9 | N | — |
| Panels/DefaultLayout.hpp:46 | browser reference px | 1144 | SETTING | editor.layout.factory.browserRefPx | LayoutFactorySettings | Editor Dev | Pref-M | Live (on Reset Layout) | 1..16384 | N | measured from the user's layout (S6-45 amendment): replaces editor.layout.factory.assetsInspectorShare, one name per value (ruling S6-32) |
| Panels/DefaultLayout.hpp:47 | Inspector-2 reference px | 392 | SETTING | editor.layout.factory.assetsInspectorRefPx | LayoutFactorySettings | Editor Dev | Pref-M | Live (on Reset Layout) | 1..16384 | N | as editor.layout.factory.browserRefPx (S6-45 amendment) |
| Panels/DefaultLayout.hpp:50 | kDefaultAssetsInspectorBandFraction | ratio | DERIVED | — | — | — | — | — | — | N | formula |
| Panels/EditorPanels.cpp:533 | split ratio clamp | 0.05..0.95 | CONSTANT | — | — | — | — | — | — | N | DockBuilder guard |
| Panels/EditorPanels.cpp:154-156 | dockspace host style | 0 rounding / border / padding | CONSTANT | — | — | — | — | — | — | N | invisible host window |
| Panels/PanelRegistry.hpp:69 | PanelVisibility default | all visible | SETTING | editor.layout.openPanelsAtStart | LayoutSettings | Editor | Pref-M | Restart | panel-name list | N | spec s7.4 |
| Panels/PanelRegistry.hpp:53-62 | permanent flag (Viewport only) | — | CONSTANT | — | — | — | — | — | — | N | the Viewport cannot close (design invariant) |
| — (new) | default named layout | — | SETTING | editor.layout.default | LayoutSettings | Editor | Pref-M | Restart | string | N | spec s7.4 |
| App/EditorApp.hpp:943 | m_playMode default | Viewport | SETTING | editor.play.launchMode | EditorPlaySettings | Editor | Pref-P | NextWorld (next Play) | PlayLaunchMode enum | N | in the layout ini today |

### Shortcuts

The ImGui-routed checks (11 files) and the raw-scancode route in `EditorAppFrame.cpp` are listed together. Library-internal keys and mouse chords are included.

| file:line | key/chord | action it triggers | context | proposed action id |
|---|---|---|---|---|
| App/EditorAppFrame.cpp:897 | Ctrl+Z (scancode) | Undo (refused in Play or during an open txn) | Global (Edit mode) | edit.undo |
| App/EditorAppFrame.cpp:898 | Ctrl+Shift+Z | Redo | Global | edit.redo (alt chord) |
| App/EditorAppFrame.cpp:898 | Ctrl+Y | Redo | Global | edit.redo |
| App/EditorAppFrame.cpp:928 | Ctrl+N | New Scene (request) | Global | file.newScene |
| App/EditorAppFrame.cpp:929 | Ctrl+O | Open Scene | Global | file.openScene |
| App/EditorAppFrame.cpp:930 | Ctrl+S | Save Scene | Global | file.saveScene |
| App/EditorAppFrame.cpp:942 | Ctrl+X | Cut entities | Global | edit.cut |
| App/EditorAppFrame.cpp:943 | Ctrl+C | Copy entities | Global | edit.copy |
| App/EditorAppFrame.cpp:944 | Ctrl+V | Paste entities | Global | edit.paste |
| App/EditorAppFrame.cpp:945 | Ctrl+D | Duplicate entities | Global | edit.duplicate |
| App/EditorAppFrame.cpp:968, :984 | Q | Select tool (gizmo off) | Viewport (focus, not while RMB) | editor.viewport.toolSelect |
| App/EditorAppFrame.cpp:965, :988 | W | Translate gizmo | Viewport | editor.viewport.toolTranslate |
| App/EditorAppFrame.cpp:966, :992 | E | Rotate gizmo | Viewport | editor.viewport.toolRotate |
| App/EditorAppFrame.cpp:967, :996 | R | Scale gizmo | Viewport | editor.viewport.toolScale |
| App/EditorAppFrame.cpp:1145, :1148 | Alt+G | Perspective view mode | Global (viewport target) | editor.view.perspective |
| App/EditorAppFrame.cpp:1146, :1149 | Alt+J | 2D view mode | Global | editor.view.ortho2D |
| App/EditorAppFrame.cpp:1158, :1163 | F | Frame selection (all if none) | Global | editor.view.frameSelected |
| App/EditorAppFrame.cpp:1159, :1165 | Home | Frame all | Global | editor.view.frameAll |
| App/EditorAppFrame.cpp:1101-1102 | W / S (held, RMB look) | Fly forward / back | Viewport (Look gesture) | editor.camera.flyForward / flyBack |
| App/EditorAppFrame.cpp:1103-1104 | D / A (held, RMB) | Fly right / left | Viewport | editor.camera.flyRight / flyLeft |
| App/EditorAppFrame.cpp:1105-1106 | E / Q (held, RMB) | Fly up / down | Viewport | editor.camera.flyUp / flyDown |
| App/EditorAppFrame.cpp:1033, :1108 | Shift (held while flying) | ×2 fly boost | Viewport | editor.camera.boost |
| App/EditorAppFrame.cpp:1032, :1060 | Alt + LMB drag | Orbit (Perspective) | Viewport | editor.camera.orbit |
| App/EditorAppFrame.cpp:1059-1062 | RMB drag / MMB drag | Look or 2D pan / 3D pan | Viewport | editor.camera.look / pan (mouse) |
| App/EditorAppFrame.cpp:1112-1113, :1122-1127 | Wheel (+RMB) | Zoom / dolly; with RMB, speed step | Viewport | editor.camera.zoom / speedStep (mouse) |
| App/EditorAppFrame.cpp:1204, :1304 | Ctrl (held during gizmo drag) | Snap | Viewport | editor.gizmo.snapModifier |
| Panels/EditorPanels.cpp:1560; App/EditorAppFrame.cpp:3771 | Ctrl+click | Toggle pick into selection | Viewport | editor.viewport.pickToggle |
| Panels/EditorPanels.cpp:1559; App/EditorAppFrame.cpp:3765 | Alt+click | Cycle-pick (2D/Play; Perspective = orbit) | Viewport | editor.viewport.pickCycle |
| Panels/EditorPanels.cpp:1925 | F2 | Rename entity | Outliner | outliner.rename (Edit menu prints F2) |
| Panels/EditorPanels.cpp:1936 | Delete | Delete selection | Outliner | outliner.delete |
| Panels/EditorPanels.cpp:2225, :2229 | Ctrl+click | Toggle row selection | Outliner | outliner.toggleSelect |
| Panels/EditorPanels.cpp:2226, :2230 | Shift+click | Range select | Outliner | outliner.rangeSelect |
| Panels/EditorPanels.cpp:2250-2254 | slow 2nd click (DoubleClickTime..1.2 s) | Rename | Outliner | outliner.slowClickRename (+ editor.outliner.slowClickMaxSeconds = 1.2) |
| Panels/EditorPanels.cpp:1313 | Ctrl+C | Copy log rows | Console | console.copy |
| Panels/EditorPanels.cpp:1311 (ImGui MultiSelect built-in) | Ctrl+A | Select all rows | Console | console.selectAll |
| Panels/AssetBrowserPanel.cpp:1048-1049 | Up / Down (repeat) | Move selection | AssetBrowser | assets.selectPrev / selectNext |
| Panels/AssetBrowserPanel.cpp:1060 | Enter | Open asset | AssetBrowser | assets.open |
| Panels/AssetBrowserPanel.cpp:184 | Double-click | Open asset | AssetBrowser | assets.open (mouse) |
| Panels/AssetBrowserPanel.cpp:1066 | F2 | Rename asset | AssetBrowser | assets.rename |
| Panels/AssetBrowserPanel.cpp:1068 | Ctrl+D | Duplicate assets | AssetBrowser | assets.duplicate |
| Panels/AssetBrowserPanel.cpp:1070 | Delete | Delete (confirm modal) | AssetBrowser | assets.delete |
| Panels/AssetBrowserPanel.cpp:563 | Escape | Cancel inline rename | AssetBrowser (Text) | assets.cancelRename |
| Panels/AssetGraphPanel.cpp:288, :290 | Down / Up (InputText callback) | Move focus-combo highlight | AssetGraph (Text) | assetGraph.focusNext / focusPrev |
| Panels/AssetGraphPanel.cpp:2535 | Enter | Apply highlighted focus row | AssetGraph | assetGraph.applyFocus |
| Panels/AssetReferenceField.cpp:242 | Double-click name | Open referenced asset | Inspector | inspector.openReference (mouse) |
| Panels/InspectorWindows.cpp:431 | Ctrl+S (RouteFocused) | Save the asset page's source | Inspector | document.save |
| Documents/ShaderEditorDocument.cpp:2118 | Ctrl+S | Save material | Graph (document) | document.save |
| Documents/ShaderEditorDocument.cpp:5021 | F (hovered, not typing) | Frame selection or content | Graph | graph.frameSelected |
| Documents/ShaderEditorDocument.cpp:3875 | F | Frame (pass canvas) | Graph (pass canvas) | graph.frameSelected |
| Documents/ShaderEditorDocument.cpp:4381 | Escape (mid-drag) | Revert canvas drag | Graph | graph.cancelDrag |
| Documents/ShaderEditorDocument.cpp:2170-2172 | Mouse X1 / X2 | Navigate back / forward through pass history | Graph | graph.navBack / navForward |
| Documents/ShaderEditorDocument.cpp:6591-6624 (lib imgui_node_editor.cpp:4437-4443) | Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+D | Copy / Cut / Paste / Duplicate nodes | Graph | graph.copy / cut / paste / duplicate |
| ThirdParty/imgui-node-editor/imgui_node_editor.cpp:4999 | Delete | Delete nodes/links (handled at ShaderEditorDocument.cpp:3824, :6553) | Graph | graph.delete |
| ThirdParty/imgui-node-editor/imgui_node_editor.cpp:3371-3373 | F / Shift+F | Library navigate (Shift = with margin) | Graph / AssetGraph | graph.frameSelected (library side) |
| ThirdParty/imgui-node-editor/imgui_node_editor.cpp:4445 | Space | CreateNode candidate (editor never accepts it) | Graph | graph.createNode (unbound today) |
| ThirdParty/imgui-node-editor/imgui_node_editor.cpp:4111-4130 | Shift / Ctrl / Alt + click/drag | Add to selection / toggle / link-select | Graph | graph.selectAdd / selectToggle |
| Documents/MeshDocument.cpp:527 | Ctrl+S | Save mesh | Document | document.save |
| Documents/SpriteDocument.cpp:213 | Ctrl+S | Save sprite | Document | document.save |
| Documents/InputActionsDocument.cpp:271 | Ctrl+S | Save input.json | Document | document.save |
| Documents/InputActionsDocument.cpp:229 | Escape | Cancel rebind capture | Input Actions | input.cancelRebind |
| Documents/InputActionsDocumentWidgets.cpp:105 | Escape | Cancel inline rename | Input Actions (Text) | input.cancelRename |
| Documents/InputActionsDocumentWidgets.cpp:692-693 | Down / Up (repeat) | Step action-row selection | Input Actions (actions column) | input.selectNext / selectPrev |
| Documents/InputActionsDocumentWidgets.cpp:710 | Left (no Alt) | Collapse action or go to parent | Input Actions | input.collapseOrParent |
| Documents/InputActionsDocumentWidgets.cpp:722 | Right (no Alt) | Expand or go to first child | Input Actions | input.expandOrChild |
| Documents/InputActionsDocumentWidgets.cpp:733 | Enter | Rebind binding/part | Input Actions | input.rebind |
| Documents/InputActionsDocumentWidgets.cpp:735 | F2 | Rename action | Input Actions | input.rename |
| Documents/InputActionsDocumentWidgets.cpp:737 | Delete | Remove action/binding/part | Input Actions | input.delete |
| Documents/InputActionsDocumentWidgets.cpp:768-769 | Down / Up | Step map selection | Input Actions (maps column) | input.mapNext / mapPrev |
| Documents/InputActionsDocumentWidgets.cpp:771 | F2 | Rename map | Input Actions (maps) | input.renameMap |
| Documents/InputActionsDocumentWidgets.cpp:772 | Delete | Remove map | Input Actions (maps) | input.deleteMap |
| Widgets/PropertyGrid.cpp:141 | Enter / KeypadEnter | Keep refused text and re-arm the field | Inspector (Text) | propertyGrid.confirm |
| Widgets/PropertyGrid.cpp:324 | Escape (mid-drag) | Revert the numeric drag | Inspector | propertyGrid.cancelDrag |
| App/EditorAppFrame.cpp:3447 | Escape / Enter | Dismiss error modal | Modal | modal.confirm / modal.cancel |
| App/EditorAppFrame.cpp:3588 | Escape | Cancel unsaved-changes modal | Modal | modal.cancel |

Related shortcut timing tunable:

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Panels/EditorPanels.cpp:2254 | slow-click rename window | 1.2 s | SETTING | editor.outliner.slowClickMaxSeconds | OutlinerSettings | Editor | Pref-M | Live | 0.4..3 | N | UE/Explorer feel (reconciled R1: one struct per prefix) |
| App/EditorAppFrame.cpp:95-137 | SDL scancode values | 4..230 | CONSTANT | — | — | — | — | — | — | N | SDL's table; the bindings become `editor.keys.*` cvars, the codes do not |

### Persistence stores

What the editor and the Hub persist today, and where:

| Store | What it persists | Where | Code | Proposed fate (s10.2) |
|---|---|---|---|---|
| User cvar archive | User-rung Archive cvars (editor.undo.*, editor.graph.*, editor.inspector.*, console.historySize, log.level) | `<project>/Saved/Config/` | ArcaneCore/src/Arcane/Base/Runtime.cpp:482, :589; enabled at App/EditorApp.cpp:368 (interactive only), written at :3495 | Stays. Machine-wide preferences (theme, fonts, keys, layouts) need the EditorUser rung, which does not exist today |
| Layout ini | ImGui windows, docking, table columns, plus 4 custom sections | `%LOCALAPPDATA%\Arcane\editor\layouts\<project-guid or "default">.ini` (App/EditorApp.cpp:1655-1667). The first run copies cwd `imgui.ini` (:1676-1678). Headless reads `<project>/Saved/verify-layout.ini` read-only (:1627) | App/EditorApp.cpp:1586 RetargetLayoutIni | Stays as layout. Named layouts go to `Saved/Layouts/<name>.ini` (spec s7.4) |
| ↳ [EditorPlayMode][State] | `Mode=` PlayLaunchMode | layout ini | App/EditorApp.cpp:94-165 | Move to `editor.play.launchMode` (it is a preference, not layout) |
| ↳ [EditorViewport][Camera] | Mode, Ortho (center, halfHeight), Orbit (pivot, yaw, pitch, dist, **fovY**), **Speed**, **Grid** (show, plane), **GizmoSize** | layout ini | Viewport/ViewportSettings.hpp:72-89; App/EditorApp.cpp:173-255 | Bold fields become `editor.viewport.*`, `editor.camera.*` and `editor.gizmo.size` with a one-time import. Pose (Mode, Ortho, Orbit pivot/yaw/pitch/dist) stays in the ini |
| ↳ [EditorPanels][Visibility] | `<Panel>=0/1` per non-permanent panel | layout ini | App/EditorApp.cpp:259-320 | Stays as layout; `editor.layout.openPanelsAtStart` seeds it |
| ↳ [EditorInspector][Instances] | `Ids=` instance list; `Filters=` excluded kinds per instance (pins/history deliberately NOT persisted) | layout ini | Panels/InspectorWindows.cpp:494-560 | Stays as layout |
| Recent projects (shared with the Hub) | `{version:1, items:[…]}` newest first; editor touches generic JSON | `%LOCALAPPDATA%\Arcane\hub\recents.archub` | Project/RecentProjects.hpp:5-26; ArcaneHub/src-tauri/src/paths.rs:38 | Stays (a cross-process document) |
| Recent scenes | `{version:1, scenes:[…]}`, max 10 | `<project>/Saved/recent_scenes.json` | Project/SceneRecents.hpp:3-6, :34 | Stays (a document); the max becomes a cvar |
| Texture import settings | the `.meta` "texture" block: format, sRGB, generateMips, maxSize | `<source>.png.meta` (merge-write) | Panels/TextureImportSettings.cpp:61-78 | Per-asset overrides stay. The defaults become `assets.import.texture.*` (rows below) |
| Thumbnails | material/mesh 64 px PNGs | `<project>/Saved/Thumbnails/<guid>.png` | App/EditorApp.cpp:1017; Project/MaterialPreviewHarvester.cpp | Cache, not a setting |
| Undo spill | payloads above spillThresholdKB | `<project>/Saved/UndoCache` (wiped on open/close) | App/EditorAppProject.cpp:2270; App/EditorApp.cpp:3445 | Cache |
| Hub cover | composited screenshot | `<project>/Saved/AutoScreenshot.png` | App/EditorApp.cpp:1888 | Artifact |
| Diagnostics | crash/hang reports | `<project>/Saved/Diagnostics` | App/EditorApp.cpp:1729 | Artifact |
| Editor lock | one editor per project | `<project>/Saved/editor.lock` (ArcaneCore EditorLock) | App/EditorApp.cpp:3502; App/EditorAppProject.cpp:2293 | Runtime state |
| Graph node layout | node positions inside the `.arcmat`; every `ed::Config` has `SettingsFile = nullptr` | the document itself | Documents/ShaderEditorDocument.cpp:3358, :4824; Panels/AssetGraphPanel.cpp:1323 | Document data |
| Hub settings | defaultProjectDir, launchBehavior, projectView, projectSort, projectSortDesc, confirmDelete | `%LOCALAPPDATA%\Arcane\hub\settings.archub` (+ engines.archub, recents.archub) | ArcaneHub/src-tauri/src/paths.rs:46; settings.rs:72-136 | Stays in the Hub (Rust; no cvar registry) |
| Not persisted (session only) | inspector pins/history, asset activity log, console toggles/search, physics overlay, gizmo mode/space, asset-browser search/selection | — | Panels/InspectorHost.hpp:9; Panels/AssetActivityLog.hpp:18; Scene/PhysicsOverlay.hpp:7 | Candidates above where marked |

Tunables inside the stores that are not already listed under the viewport and layout areas:

| file:line | symbol | value (unit) | verdict | proposed cvar name | struct | aud | scope | apply | range | det | why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ArcaneAssetPipeline/src/Arcane/AssetPipeline/TextureMetaSettings.hpp:29 | format default | Auto (=Bc7) | SETTING | assets.import.texture.format | TextureMetaSettings | Editor (pipeline-declared; arccook reads it too) | Project | Live (next cook) | {Auto, Bc7, Rgba8} | N | changes cooked bytes, so goldens move |
| …TextureMetaSettings.hpp:37 | srgb default | true | SETTING | assets.import.texture.srgb | TextureMetaSettings | Editor | Project | Live | bool | N | — |
| …TextureMetaSettings.hpp:38 | generateMips default | true | SETTING | assets.import.texture.generateMips | TextureMetaSettings | Editor | Project | Live | bool | N | — |
| …TextureMetaSettings.hpp:39 | maxSize default | 0 (unlimited) | SETTING | assets.import.texture.maxSize | TextureMetaSettings | Editor | Project | Live | 0..16384 | N | — |
| App/EditorApp.cpp:1655-1656 | layout dir | %LOCALAPPDATA%\Arcane\editor\layouts | CONSTANT | — | — | — | — | — | — | N | rung location (s11.1), not a value |
| App/EditorApp.cpp:98-99, :259-260; Viewport/ViewportSettings.hpp:72-73; Panels/InspectorWindows.cpp:507-508 | ini section names | "EditorPlayMode"/"State", … | CONSTANT | — | — | — | — | — | — | N | file format |

### Existing cvars (e)

| name | file:line | type / default / range | flags | consumer | gap against the spec |
|---|---|---|---|---|---|
| editor.undo.maxSteps | App/UndoSettings.cpp:13 | Int32 100, 1..10000 | Archive | ReadUndoLimits (string Find) | fallback shadow copy at :33; no audience/scope/apply metadata yet |
| editor.undo.byteBudgetMB | App/UndoSettings.cpp:16 | Int32 512, 16..65536 | Archive | same | shadow at :34 |
| editor.undo.spillThresholdKB | App/UndoSettings.cpp:19 | Int32 256, 16..1048576 | Archive | same | shadow at :35 |
| editor.inspector.materialPreviewFraction | Documents/ShaderEditorDocument.cpp:73 | Float32 0.45, 0.2..0.8 | Archive | :93 (Find on every call) | shadow at :94 |
| editor.inspector.nodePageMinTextRun | Documents/ShaderEditorDocument.cpp:692 | Int32 16, 0..256 | Archive | :703 | shadow at :704 |
| editor.graph.showPinLegend | Documents/ShaderGraphPinLegend.cpp:20 | Bool true | Archive | :87 read, :94 Set(SetBy::User) | the UI writes it directly; already the s6.3 pattern |
| editor.inspector.assetThumbMinPx | Panels/AssetInspectorSource.cpp:39 | Int32 64, 32..140 | Archive | :49 | max is tied to kAssetPageThumbSize (:37); shadow at :50 |
| editor.inspector.assetThumbHeightFraction | Panels/AssetInspectorSource.cpp:42 | Float32 0.30, 0.1..0.6 | Archive | :55 | shadow at :56 |
| editor.graph.fitMaxZoom | Widgets/GraphFit.cpp:24 | Float32 1.0, 0.1..2.0 | Archive | :54 | shadow fallback at :54 |
| editor.graph.fitMinZoom | Widgets/GraphFit.cpp:35 | Float32 0.5, 0.1..2.0 | Archive | :55 | shadow at :55 |
| editor.automation.windowedFrameCapture | App/EditorAppFrame.cpp:1608-1620 (CVarDesc) | Bool false | Dev \| UserSettable | :1628 | correctly not Archive; "UserSettable" becomes the PlayerSafe derivation, which looks wrong for an Editor-audience automation switch |
| editor.remote.enable | App/EditorAppFrame.cpp:1677 (ARC_CVAR; Editor audience, PreferencesMachine scope) | Bool true | Dev | EditorApp::UpdateRemoteSession, each frame | added post-freeze (Servitor P0 task 11): arms the editor's remote surface (or --remote does); a published true -> false stops it until true again. Not a frozen name: ArcaneTests does not compile its file |
| console.historySize | ArcaneCore/src/Arcane/Config/CVarRegistry.cpp:123 | Int32 64, 1..1024 | Archive | ConsoleModel | module "engine"; it is an editor-console preference |
| log.level | ArcaneCore/src/Arcane/Base/Log.cpp:258 | Int32 = Init arg, 0..6 | Archive \| Dev | callback | fine |
| diagnostics.drawMarkers | ArcaneClient/src/Arcane/Render/GpuInstrumentation.cpp:27 | Bool false | Dev | GpuDrawMarkersEnabled | no consumer scope yet (per its comment) |
| render.meshCull | ArcaneClient/src/Arcane/Render/Nri/nodes/MeshCullNode.cpp:21 | Bool kMeshCullEnabled | Dev | MeshCullFrustumEnabled (Find per call) | per-frame string lookup |
| tests.rangedProbe | ArcaneTests/src/CVarRegistryTest.cpp:428 | Int32 5 | — | test | test-only |

---

### Should NOT be exposed

- **File, ini and wire formats; payload and clipboard ids**
  - ini section names: `ViewportSettings::kIniType/kIniName` (Viewport/ViewportSettings.hpp:72-73), `kPanelsIniType/Name` (App/EditorApp.cpp:259-260), `kInstancesIniType/Name` (Panels/InspectorWindows.cpp:507-508).
  - Format versions: `RecentProjects kFormatVersion` (Project/RecentProjects.cpp:30), `SceneRecents::kFormatVersion` (Project/SceneRecents.hpp:32), Hub `STATE_FORMAT_VERSION` (ArcaneHub/src-tauri/src/store.rs:34).
  - Drag-drop and clipboard keys: `kAssetDragType "ARCANE_ASSET"` (Panels/AssetPanelModel.hpp:81), `kDragPayload "ARC_INPUT_ROW"` (Documents/InputActionsDocumentWidgets.cpp:21), `kOutlinerDragType` (Panels/EditorPanels.cpp:1572), `kEntityClipboardKey` (Scene/EntityClipboard.hpp:17).
- **OS, hardware and ABI limits**
  - SDL scancode values (App/EditorAppFrame.cpp:95-137). Rebinding changes which code an action uses, never the codes themselves.
  - `D3D12SDKVersion = 619` (main.cpp:43) and `kAppUserModelId` (main.cpp:36).
  - Path and filename rules: `kCreateNameMaxPathChars 240` (Panels/CreateAssetDialog.hpp:332), `kDenied` filename chars (:351).
  - Texture limit: Max Size 16384 (Panels/TextureImportSettings.cpp:74).
  - Windows API values: SHFileOperation flags (Project/OsShell.cpp:198), ShellExecute return codes (Project/OsShell.cpp:71-73).
- **Library mirrors and API contracts**
  - `kNavigationZoomMargin 0.1` (Widgets/GraphFit.cpp:44) and `kGraphLinkChannel 7` (Widgets/GraphWire.hpp:72).
  - `ed::Config::SettingsFile = nullptr` (Documents/ShaderEditorDocument.cpp:3358, :4824; Panels/AssetGraphPanel.cpp:1323). This is the no-canvas-persistence ruling.
- **Math identities and numeric guards**
  - `kPi` (Widgets/GraphWire.hpp:148) and the Bezier weights 3.0 (Widgets/GraphWire.hpp:84-85).
  - The sRGB and WCAG transfer constants (Widgets/EditorWidgets.cpp:1374-1382; Widgets/EditorTheme.hpp:64-65).
  - The pitch lock 90−1e-3 (Viewport/ViewportSettings.hpp:69; Viewport/EditorCamera.cpp:73) and the FOV bounds 1..179 (Viewport/ViewportSettings.hpp:63-64).
  - Epsilons: Widgets/GraphGridPhase.hpp:97, Widgets/GraphNodeLod.hpp:79, Panels/InspectorFields.cpp:390. The framing radius floor at Viewport/EditorCamera.cpp:131.
- **ID spaces, sentinels and fixed GUIDs**
  - Node and pin id bases: `kPassOutputNodeId 900000 / kPassSceneNodeId / kPassOutputLinkId / kPinInBase / kPinOutBase` (Documents/ShaderEditorDocument.cpp:297-303), `kGraphPinIdBase 1<<32` (Panels/AssetGraphPanel.cpp:650), `TextKey <<56` (Documents/ShaderEditorDocument.hpp:1203).
  - Sentinels: `kUnreachableStateId` (Scene/SceneSession.hpp:76).
  - Fixed preview GUIDs: Documents/MaterialSpherePreview.hpp:29, Documents/MeshDocument.cpp:51.
  - Window-id contracts: `kAssetsInstanceId 1` (Panels/InspectorHost.hpp:56), `"###Inspector"` (Panels/InspectorWindows.hpp:26).
- **Enum and array arity**
  - Kind and choice counts: `kAssetKindCount 13` (Panels/AssetPanelModel.hpp:77), `kCreateAssetKindCount 7` (Panels/CreateAssetDialog.hpp:65), `kPrimitiveMeshSourceCount 5` / `kMaterialSurfaceCount 3` (Panels/CreateAssetDialog.hpp:200, :248), `kSystemPhaseChoiceCount` / `kSystemRoleChoiceCount` (Project/ClassTemplates.hpp:36-37).
  - Fixed shapes: the 4-segment selection key (Documents/InputSelectionKey.hpp:40), the 3 grid levels (Viewport/ViewportGrid.hpp:87), and `kInspectorKinds` (Panels/InspectorKinds.hpp:29).
- **Test, automation and frame-protocol values**
  - `m_scriptedOpenFocusFrames 3` (App/EditorApp.cpp:1272).
  - The input probe offsets of 40 px (Documents/InputActionsDocumentWidgets.cpp:306, :445).
  - `m_graphExit` codes (App/EditorApp.cpp:3034, :3067; App/EditorAppFrame.cpp:272).
  - The PropertyGrid hold of 3 frames (Widgets/PropertyGrid.cpp:96) and the zero-dt fallback of 1/60 s (Documents/InputActionsDocument.cpp:231).
- **Cross-process contracts**
  - The Hub cover: 512 px (App/EditorApp.cpp:1962) and the 2 MiB cap (ArcaneHub/src-tauri/src/resolve.rs:109).
  - The recents.archub path and schema (Project/RecentProjects.hpp:9-17).
  - `kDiscoveryExtensions` (Project/ContentDiscovery.hpp:64) must match the cook pipeline.
- **Values whose change would be a bug**
  - `Theme::kNone` (Widgets/EditorTheme.hpp:163).
  - The dock split clamp 0.05..0.95 (Panels/EditorPanels.cpp:533).
  - The permanent Viewport panel (Panels/PanelRegistry.hpp:54).
  - The dockspace host's zero style (Panels/EditorPanels.cpp:154-156).

### Notes

- **Shortcut counts differ from the spec.** The spec says "~40 checks in 8 files". The code has about 49 ImGui-routed key checks across 11 files:
  - EditorAppFrame, InputActionsDocument, InputActionsDocumentWidgets, MeshDocument, ShaderEditorDocument, SpriteDocument, AssetBrowserPanel, AssetGraphPanel, EditorPanels, InspectorWindows, PropertyGrid.
  - Plus 22 raw-scancode bindings in `EditorAppFrame.cpp` and 7 key paths inside the vendored imgui-node-editor.
- **There are two input routes.** Global, viewport and camera keys read SDL **scancodes** through `InputSnapshot` (App/EditorAppFrame.cpp:895-1204). Everything else reads ImGui keys. The action registry (s7.2) must resolve chords for both.
  - Scancodes are physical positions: on AZERTY, "W" is the key labelled Z. Decide whether `editor.keys.*` stores scancodes or ImGui keys.
- **Node-editor keys live in ThirdParty** (imgui_node_editor.cpp:3371, :4437-4445, :4999). Rebinding them needs a local fix or a disable-shortcuts hook (`ed::EnableShortcuts`) plus our own handling.
  - Space raises a `CreateNode` candidate that the editor never accepts. It is a dead key today.
- **The menus already drift from the code:**
  - Edit > Redo prints only "Ctrl+Y" (Panels/EditorPanels.cpp:275), but Ctrl+Shift+Z also redoes.
  - Edit > Rename/Delete print F2/Del (:302, :304), but those keys only fire while the Outliner has focus.
  - Edit > Select All has no chord, while the Console uses Ctrl+A.
  - The spec's "menus show the current chord" rule fixes all of this.
- **Hard pixel sizes ignore font size and scale.** About 60 sizes, paddings and font-px constants (12/13/14/24 px fonts, 24 px rows, dialog widths) are absolute. `editor.ui.scale` (s7.3) will not reach them unless they become DERIVED from `fontSize × ratio` or `scale × base`. I marked the font sizes DERIVED and the paddings SETTING Dev. Consider one `editor.ui.scale` multiplier rather than about 100 individual Dev cvars.
- **Colours drift from the theme tokens:**
  - Panels/EditorPanels.cpp:1348 uses (1.0, 0.45, 0.45) for errors, not `Theme::kError`.
  - :1243 uses 0.80 gray, not `Theme::kText`.
  - The mesh preview light direction (Documents/MeshDocument.cpp:423) differs from the material preview and harvester's (0.45, 0.7, 0.8).
  - The camera speed clamp is spelled twice (Viewport/EditorCamera.cpp:114 and Viewport/ViewportSettings.hpp:66-67).
- **Fallback shadow copies.** Every existing editor cvar re-spells its default as a `ReadInt`/`ReadFloat` fallback and does a string `Find` on every read. Both break s10.2: no shadow copies, and reads come from snapshots.
- **Graph canvases cannot apply style live.** `ApplyGraphCanvasStyle` and `ApplyZoomLevels` latch at `ed::CreateEditor`, so "Restart" here means "reopen the document". A binding could re-apply `ed::GetStyle()` when the snapshot changes to make these Live.
- **Gizmo snap is outside this scope but editor-facing.** Its defaults live in ArcaneClient (Gizmo.hpp:94-97). The editor default-constructs `GizmoSnap` and only sets `enabled` from Ctrl. There is no snap UI.
- **Gaps found:**
  - There is no unfocused/background frame throttle (only a 1 ms minimized sleep). Candidate: `editor.perf.backgroundFps`.
  - There is no `ScaleAllSizes` or DPI handling.
- **ImGui io/style defaults the editor never sets** are natural Preferences candidates (proposed `editor.ui.input.*`, Pref-M, Live):
  - `MouseDoubleClickTime` (used by the slow-click rename) and `HoverStationaryDelay` (used at Panels/AssetGraphPanel.cpp:2394).
  - `KeyRepeatDelay`, `KeyRepeatRate`, `MouseDragThreshold`.
  - `IniSavingRate` (layout autosave cadence).
- **Out of scope but noted:**
  - The 2D ortho near/far ±1000 lives in ArcaneClient's `ViewTransform`.
  - `kSettleIntervalMs` / `kHangMainSeconds` are HostConfig automation values.
- **New-asset template content** (the default node positions and colour at App/EditorAppProject.cpp:1708-1716 and Documents/ShaderEditorDocument.cpp:3999-4011) is better moved to shipped template assets than to cvars.
- **The Hub cannot join the cvar registry** (Rust/Tauri, separate process). Its `settings.archub` and `theme.css` stay its own stores. If Preferences should surface them, it needs a JSON bridge.
- **The comment at Panels/AssetReferenceField.cpp:22** ("a size, not a tunable") conflicts with s10.1's rule. I classified it SETTING Dev and flagged it for the allow-list review.
- **Totals:**
  - Theme: 31 tokens (27 SETTING, 3 DERIVED aliases, 1 CONSTANT), plus about 27 domain-palette and alpha rows.
  - Style metrics: 5.
  - Stores: 4 ImGui ini sections, plus user cvar archive, recents, scene recents, texture `.meta`, Hub settings, and caches.
  - Existing cvars: 17, of which 12 are editor-declared (`editor.remote.enable` added post-freeze, Servitor P0 task 11; the freeze-time "16, of which 10" undercounted the 11 editor rows).
