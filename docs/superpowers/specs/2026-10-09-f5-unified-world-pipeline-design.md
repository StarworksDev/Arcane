# F5 unified world pipeline — design

**Date:** 2026-10-09
**Status:** APPROVED design 2026-10-09 (user). Review round 2 (C1–C17) incorporated. Plans are not yet written.
**Branch measured:** `feat/f5-unified-world` at `7e6f8881` (off `main`). Line citations below were re-read in this tree.
**Roadmap:** the F-series foundation that precedes D17 and D1 (`docs/superpowers/specs/2026-10-05-arcane-1.0-roadmap.md`).
**ABI:** `kGamePluginABIVersion` is 54 (`ArcaneCore/src/Arcane/Plugin/PluginABI.hpp:1131`). Any bump is numbered against `main` at execution, not in this spec.

Binding rulings: Q1–Q7, the adopted outside-review amendments, and review rulings C1–C17 (section 12). Material direction of 2026-10-02, with its exclusion of parts 3–4 superseded: F5 does all of it. Facts measured 2026-10-09, re-verified here before citation.

---

## 0. Status

**SPEC APPROVED 2026-10-09 by the user (final written-spec review, after two Codex reviews and two outside reviews).** Sections 1–4 of the design discussion, the adopted amendments, and Q7 are closed. Review rulings C1–C17 are part of that design. Plans are written one at a time (plan 1 structure, plan 2 materials, plan 3 sprites); no product code lands with this spec.

Roadmap placement. D1 is the render foundation (clustered deferred, GGX, shadows, GTAO, TAA, the upscaler, and the Surface domain as a finished lighting model). D17 is large-world precision (64-bit positions, camera-relative rendering, origin rebasing). D3 is the render thread. F5 is the F-series foundation those rows attach to: one world pipeline, one material contract, one extract door, the deferred pass slots declared, and the TAA inputs reserved. D1, D17, and D3 each get built once, on this path.

What Source 2 / Deadlock does, and where this spec matches or diverges, is section 3.5. The standing rule (2026-09-04, restated in `docs/research/2026-09-16-direction-and-sequencing.md`) requires that of every design.

---

## 1. Goal and non-goals

### 1.1 Goal

World-space 2D draws through the 3D path. A sprite is a quad instance in the GPU scene, shaded by the same Surface material a mesh uses, depth-tested against the same depth buffer, in one world pass. Screen-space UI, text, and editor/debug chrome stay a later pass, the way Unreal keeps Slate/UMG and Source 2 keeps Panorama off the world colour.

Three contracts fall out of that, and F5 ships all three:

1. **One frame shape.** A game-thread extract produces an immutable packet. GPU-scene build, cull, the world pass, post, tonemap, the overlay, and screen UI read the packet and nothing from the ECS.
2. **One Surface material.** Domain, shading model, and blend mode are separate settings. Sprite and mesh are vertex-factory permutations of that one material. The output contract is the Unreal-shaped set, so D1's deferred lighting consumes it without a second material model.
3. **One sprite order.** Quantized view depth, then `sortingLayer`, then `orderInLayer`, then an optional per-camera sort axis, then a stable id. Masked and translucent sprites share that key, inside one back-to-front list with translucent meshes and, later, particles (C7).

Aphelyon is the product and is 2D. Its one sprite, viewed through its orthographic camera, looks the same after F5 as it does today (Q1). The old world path is deleted only after that parity holds (section 7).

### 1.2 Non-goals

F5 leaves these to the roadmap row named with them.

| Left to | What |
|---|---|
| D3 | A render thread, and parallel command recording. The packet is the seam D3 will move; F5 records the graph on the game thread, as today. |
| D1 | GGX / PBR lighting, the deferred G-buffer and its lighting pass, cascaded and local shadows, GTAO, TAA, the upscaler. F5's lit shading is Lambert from BaseColor. |
| D1 | Implementing TAA jitter or a velocity target. Both are reserved (G2). |
| D17 | 64-bit world positions, a moving render origin, tile-and-offset versus rebasing. F5's origin is the constant 0. |
| D11 / the tilemap epic | Tilemaps. No component, asset, or draw. A chunk row is reserved. |
| D1 | Shadow maps. A `casts-shadow` flag is reserved on sprite rows and is not read. |
| D7 | A UI material domain, and replacing ImGui. Screen UI stays the existing `gameui` / `imgui` nodes. |

Also out of this arc, and not given a roadmap row here: selecting reverse-Z (the depth convention in section 3.1 is the single switch a later change flips), MSAA, a second simultaneous world camera in one graph, GPU sorting of sprites, and any change to Manifold2D or Box3D.

---

## 2. Where it stands

Measured on this branch at `7e6f8881`. One function declares every host frame: `DeclareGraphFrame` (`ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp:1231`). `RenderGraph::AddNode` appends, and declaration order is execution order (`NriGraphContext.cpp:1486-1488`).

### 2.1 Frame order today

| Order | Node | Where it is declared | What it does |
|---|---|---|---|
| 1 | `batch2d` | `NriGraphContext.cpp:1277` | Creates the linear canvas and is the only node that clears it (`Batch2DNode.cpp:46-51`, `1395-1405`). No depth (`depthFormat = UNKNOWN`, `Batch2DNode.cpp:1313`). An empty batch still clears (`Batch2DNode.cpp:1407-1408`). |
| 2 | `gpuscene-sync` | `GpuSceneSyncNode.cpp:33` | Uploads rows the CPU already built. It does not walk the registry. |
| 3 | `mesh-cull` | `MeshCullNode.cpp:150` | Fills indirect counts. `numthreads(64)` (`MeshCullNode.hpp:29-30`). Dispatch only when `registryReady` and `rowCount != 0` (`MeshCullNode.hpp:39-42`). |
| 4 | `mesh` | `MeshNode.cpp:1301` | Creates `depth` (`MeshNode.cpp:1305-1311`), writes colour and depth (`MeshNode.cpp:1317-1318`), clears **depth only** (`MeshNode.cpp:942-957`). |
| 5 | readback nodes | `GpuSceneSyncNode.cpp:84` and `:124`, armed from `MeshNode.cpp:1345-1350` | Host copies, only when armed. |
| — | `depth` placeholder | `NriGraphContext.cpp:1352-1364` | A depth transient with no attachment, only when a frame asks for depth and has no mesh pass. |
| 6 | `grid` | `NriGraphContext.cpp:1392-1397` | The 3D grid. Depth-tested `LESS_OR_EQUAL`, depth write off (`GridNode.hpp:7-9`, `GridNode.cpp:363-375`). Colour goes into the canvas, so the post chain and the tonemap grade it. |
| 7 | `post0..` | `NriGraphContext.cpp:1412-1437` | Fullscreen passes (`FullscreenNodes.cpp:1055`). |
| 8 | `tonemap` | `NriGraphContext.cpp:1445` | Scene colour to the swapchain or the offscreen target (`FullscreenNodes.cpp:1506-1526`). |
| 9 | `gameui` | `NriGraphContext.cpp:1474-1475` | Game HUD on the tonemap target. The runtime never sets it (`NriGraphContext.cpp:1469-1472`). |
| 10–12 | `pick`, readback, outline | `NriGraphContext.cpp:1494-1507` | Its own id and depth targets. Clears both (`PickOutlineNodes.cpp:400`). |
| 13 | `imgui` | `NriGraphContext.cpp:1521-1522` | Host chrome on the tonemap target. |
| 14 | `capture` | `NriGraphContext.cpp:1524-1576` | Backbuffer readback. |

Attachments are load/store. The graph has no declarative clear (`NriGraphContext.cpp:1235-1252`). `Batch2DNode::Record` calls `CmdClearAttachments` on the colour plane after the raster pass is open (`Batch2DNode.cpp:1395-1405`). `MeshNode` does not clear colour, so `batch2d` sits under the mesh pass (`MeshNode.hpp:40-46`, `MeshNode.cpp:949-952`). That is why 3D draws over world sprites, and why anything declared before `batch2d` would be erased.

The same declarator serves every host. The runtime (`ArcaneRuntime/src/RuntimeFrame.cpp:508-749`) arms the batcher, the mesh pass when the scene is non-empty, post, and host `imgui` (which already includes the plugin `DrawUI`). It has no grid. The editor viewport (`ArcaneEditor/src/App/EditorAppFrame.cpp:1642-1758`) is a separate offscreen graph: scene batch, mesh, the 3D grid only in Edit + Perspective + show-grid (`EditorAppFrame.cpp:2097-2119`), post, `gameui` only in Play, pick/outline in Edit. Editor chrome (`EditorAppFrame.cpp:4016-4035`) sets `imgui` only; `DeclareGraphFrame` still emits an empty `batch2d` (the clear) then tonemap then `imgui`. Thumbnails and the shader-editor preview are their own offscreen graphs (checkerboard rects, an optional sprite quad, an optional post chain, an optional mesh).

### 2.2 Who submits 2D

`Batcher2D` is device-less CPU recording (`Batcher2D.hpp:190-200`). `Drain` stable-sorts by `layer(16) | order(16) | material(16) | textureSlot(16)` (`Batcher2D.cpp:363-368`, `424-431`). `worldSpace` is not in the key (`Batcher2D.cpp:373-381`). Overlapping translucency at the same `(layer, order)` is not a stable order: texture slots are first-use order inside one `Begin` (`Batcher2D.hpp:240-248`).

Screen primitives store `z = 0` and `worldSpace = false`. `QuadWorld` / `CircleWorld` store caller corners and `worldSpace = true` (`Batcher2D.cpp:248-279`, `416-422`). Screen `z` is hardcoded `0` in `data/shaders/sprite.hlsl:57-59`. World `z` is `mul(viewProj, pos)` (`sprite.hlsl:55-56`). The batch node has no depth attachment, so that clip `z` is not tested. The built-in sprite pipelines are alpha-over with depth write off (`Batch2DNode.cpp:1315-1316`).

| Producer | Space | Sort |
|---|---|---|
| Sprites (`RenderSubmissionSystem`, `RenderSystems.hpp:47-250`) | World metres. `QuadWorld` / `CircleWorld`. | `SetLayer(sortingLayer, orderInLayer)` cast `int32` to `uint16` (`RenderSystems.hpp:159-160`). Fields: `Components.hpp:150-151`. |
| 2D editor grid (`ViewportGrid.cpp:169-233`, called `EditorAppFrame.cpp:1883-1887`) | Screen. World lines projected, then `Line`. | Layer `(0, 0)`, before sprites. Edit + 2D + show-grid. |
| Physics debug (`EditorAppFrame.cpp:1893-1926`) | Screen, through `Affine2D`. | Does not call `SetLayer`. Inherits the previous layer. After sprites. |
| Camera rect (`EditorAppFrame.cpp:1950-1981`) | Screen. | No `SetLayer`. Edit only. After sprites. |
| Material / shader previews | Screen `Rect` and `QuadMaterial`. | Default layer `(0, 0)`. |
| Glyph / MSDF text | Screen API (`Batcher2D.cpp:182-191`). | No production caller under `ArcaneClient/src`. Tests call `Glyph`. |
| Transform gizmo | Not the batcher. Pixel `GizmoDrawSink`, painted as viewport foreground (`EditorAppFrame.cpp:1986-1990`). | Already after the scene image. |
| 3D reference grid | `GridNode`, not the batcher. | After `mesh`, before post. |
| Game / editor ImGui | `ImDrawData` after tonemap. | ImGui's own order. |

Runtime submission inside one `Begin`: globals, `SetViewProjection(View())`, `SubmitRender` (`RuntimeFrame.cpp:521-548`). No grid, physics overlay, or camera rect. Editor `SubmitSceneToBatcher` (`EditorAppFrame.cpp:1857-1984`): globals, view-projection, optional 2D grid, `SubmitRender`, optional physics overlay, optional camera rect.

Sprite shapes today (`Components.hpp:133-138`, `RenderSystems.hpp:167-208`): `Rect` is a textured quad; `Circle` is an SDF disc; `Capsule` is a band quad plus two discs. Circle and capsule ignore the sprite asset and draw a 1×1 m base times scale, so they stay on the collider.

Sprite cull is `MainVisibleSet` (`views[0]`). A null set culls nothing (`RenderSystems.hpp:57-63`).

There is no `Tilemap` type under Arcane `*.hpp` / `*.cpp`.

### 2.3 The mesh path

One GPU-scene row per `(entity, mesh section)`, 240 bytes, `static_assert` (`GpuSceneTypes.hpp:47-67`), mirrored by `data/shaders/gpu_scene.hlsli:15-29`. The row carries `model`, `prevModel`, CPU normal columns, world AABB (`boundsMax.w` = alpha cutoff), `baseColor`, `materialSlot`, `batch`, `flags`. The batch key is `(mesh, section, blend, twoSided)` (`GpuSceneTypes.hpp:69-75`). `MaterialBlendMode` is `Opaque = 0`, `Masked = 1`, `Transparent = 2` (`MaterialBlendMode.hpp:7-12`). F5 keeps those numeric values; section 5 names value 2 Translucent.

`PrepareSceneForRender` (`GpuSceneHost.hpp:87-110`) builds the visible set or sets, syncs the mirror, and builds the frame. `GpuSceneSync` walks the registry (`GpuSceneSync.hpp:92-111`, `203-227`). `BuildGpuSceneFrame` (`GpuSceneSync.hpp:285-391`) does not: it reads the mirror, the `VisibleSet`, and `MeshTable`.

Opaque and masked rows become indirect batches, opaque before masked, nearest first (`GpuSceneSync.hpp:349-357`). Transparent rows skip those lists and become direct draws sorted by `renderOrder` ascending, then `projectedDepth` descending, then entity id, then mesh guid, then section (`GpuSceneSync.hpp:380-388`). `projectedDepth = -viewCenter.z + depthSortBias` (`GpuSceneSync.hpp:324-328`). Ad-hoc rows are direct, submission order, opaque one-sided (`MeshNode.cpp:1217-1239`). One raster pass, six pipelines (`MeshNode.hpp:11-16`, `MeshNode.cpp:161-183`). Opaque and masked write depth. Transparent tests and does not write. Compare is `LESS` (`MeshNode.cpp:823-832`). Clear value is `1.0` (`MeshNode.cpp:77-83`).

Depth is `[0, 1]`, forward-Z (`SceneCamera.hpp:120-129`). `glm::perspectiveRH_ZO` (`SceneCamera.hpp:153-156`). Default format `D32_SFLOAT`. Canvas default `RGBA16_SFLOAT`. Both latched once per process (`NriGraphContext.hpp:286-293`). `mesh.hlsl` is one directional light, Lambert plus constant ambient, one albedo (`mesh.hlsl:184-206`). The light is `render.mesh.defaultLight.*`. There is no light component (`RuntimeFrame.cpp:676-681`).

### 2.4 Materials

`MaterialSurface` is `Fullscreen`, `Sprite`, `Mesh` (`MaterialSource.hpp:72`). There is no Surface value.

| Model | Compiles |
|---|---|
| Built-in 2D ids 0/1/2 (`Batcher2D.hpp:208-213`) | Offline `sprite`, `circle`, `msdf`. Alpha-over, no depth. |
| `.arcmat` kind `sprite` | Template `materials/sprite_material.hlsl` (`MaterialSource.cpp:333-334`), registered on the batcher. |
| `.arcmat` kind `fullscreen` | The post chain. |
| `.arcmat` kind `mesh` | **No template.** `MaterialTemplateFile` `ARC_ENSURE`s and falls back (`MaterialSource.cpp:313-334`). `GenerateGraphSnippet` refuses `Mesh` the same way (`MaterialGraph.cpp:480-482`). Drawn by the six fixed `mesh*.hlsl` pipelines plus a bindless albedo slot. |

The graph's output node is one colour (`MaterialGraph.cpp:86-88`). `VertexColor` and `SpriteTexture` are gated to the sprite surface (`MaterialGraph.cpp:107-109`, `116-118`).

### 2.5 Two cameras in one frame

Editor Edit already uses one view for both: the editor camera (`EditorAppFrame.cpp:1685-1686` and `1873`).

The runtime and Editor Play do not. Sprites use `Runtime::View()`, which `PushSceneCamera` sets from the first active orthographic camera (`RuntimeApp.cpp:846-861`, `SceneCamera.hpp:57-112`). Meshes use `ActivePerspectiveSceneCamera` when one exists (`RuntimeFrame.cpp:541-542`, `EditorAppFrame.cpp:1687-1688`). `SameView` false produces two CPU visible sets (`GpuSceneHost.hpp:93-97`). The comment on that function already says F5 unifies the passes under one view (`GpuSceneHost.hpp:13-14`).

`ReferenceProject/Content/scenes/main.arcscene` (the `runtime-scene` golden) has an active orthographic camera (`:12-18`), an active perspective camera (`:292-298`), sprites, two `MeshRenderer`s, and a `PostProcess`. `f3_cull_blend.arcscene` (the `f3-cull-blend` golden) has the same pair of active cameras (`:27-33`, `:66-72`) and twenty mesh renderers, no sprites. Aphelyon's `Content/scenes/test.arcscene` has one active orthographic camera (`orthographicSize` 5, `:40-46`) and one `SpriteRenderer` (`:89-106`, layer 0, order 0, `Rect`).

### 2.6 Extract, views, TAA reservations

Render nodes already consume snapshots on `FrameDesc`. The registry reads that still sit on the way into that snapshot are listed in section 4.2. They are the ones that move.

One `DeclareGraphFrame` call has one canvas size and at most one mesh view/projection. `SceneVisibility::views` index 0 is the main view; the comment reserves further views for shadows and previews (`VisibilitySystem.hpp:88`). Separate editor graphs (viewport, chrome, per-document preview, thumbnails) are separate frames, not extra cameras inside one graph. No `RenderTexture` component exists.

No TAA pass, no jitter on a projection, no velocity target. Reserved already:

- `Frustum.hpp:15-16`: extract from the unjittered view, so TAA jitter cannot flip a cull.
- `GpuInstance::prevModel` is uploaded (`GpuSceneTypes.hpp:50-55`, `gpu_scene.hlsli:18`). Contract: the model the row was drawn with last frame; a new row, a full rebuild, or a teleport stores `prev == model`, and a row with `prev != model` is re-dirtied next frame (`GpuSceneSync.hpp:15-19`, `:250-263`). No shader reads `prevModel` except the struct declaration.
- `NriGraphContext.hpp:387-402`: the frame is sliced by nulling optional fields, not by a stage ordinal. The comment names TAA, with shadows, GI, SSAO and SSR, as the reason "truncate at stage N" stops being a meaningful question once those passes exist.

### 2.7 What the goldens actually picture

`scripts/golden-gate.ps1:24-37` and `:344-394`. Both D3D12 and Vulkan. `runtime-scene` and `f3-cull-blend` are backend-split. `editor-ui`, `editor-ui-perspective`, `editor-input-doc`, `editor-asset-page`, and `editor-material-page` are shared. Witnesses that assert a compare: `W1`, `W3`, `W5`, `W6` (`ArcaneTests/src/WitnessScenariosTest.cpp`) and `E2`, `E3`, `E4`, `E6` (`EditorWitnessTest.cpp`). `W5` (`WitnessScenariosTest.cpp:307`) pins `f3-cull-blend`: `diffCount == 0` at `:385`, visibility counts at `:390-406`. The comment at `:315-318` records that the boot scene has no transparent, masked, or two-sided material. `:322-329` records that the fixture is twenty single-section meshes: fourteen cubes and six planes, one one-sided/two-sided pair per blend mode.

Aphelyon `Source/Game/Aphelyon.cpp:11-39` does not spawn entities and does not submit draws. `OnDrawUI` opens one ImGui window (`:33-39`), which joins whichever host ImGui context calls `DrawUI`. Tilemaps are a Jira epic, not code (`Aphelyon/docs/jira/epic-10-tilemap-overworld.md:10-16`: "the engine cannot render a map at all"). One `Cube.arcmesh` exists under Aphelyon's `Content/meshes/`. No second scene.

---

## 3. Architecture

### 3.1 The frame

End state, one graph:

```
extract (game thread)
  -> immutable RenderFramePacket
  -> GPU-scene build
  -> cull
  -> world pass          declarative clear, one depth
  -> post                existing chain, scene colour only
  -> tonemap             existing operator, scene colour only
  -> overlay pass        reads scene depth; after tonemap
  -> screen UI           game HUD, pick/outline, host ImGui
```

This diagram is the end state. Plan 3 is the frame that has one world raster pass with the sprites in it. Plan 1's graph is the arrangement in section 8.1: the same node order as today, the declarative clear on the first world node, sprites still in `batch2d`.

The end-state world pass is one raster pass with one depth target. Its colour load-op clears the canvas to `render.clearColor`. Its depth load-op clears to the depth-convention clear (below). `CmdClearAttachments` is gone for those two planes (`Batch2DNode.cpp:1405` and `MeshNode.cpp:957` are the calls that die). The graph gains a per-attachment load op and clear value; the seam documented at `NriGraphContext.cpp:1235-1252` is the one this closes. Pick's own targets (`PickOutlineNodes.cpp:400`) may use the same load op in the same plan. Their pixels stay as they are.

Draw order inside the world pass is plan 3 (C7). Plan 1 stays the arrangement in section 8.1.

1. Opaque and masked meshes, front-to-back, indirect, existing order (opaque before masked, nearest first, `GpuSceneSync.hpp:349-357`). Compare is the convention's nearer-wins op. Depth write on. Ad-hoc opaque rows draw with them. Sprites do not enter these batches, and they do not enter `mesh-cull`.
2. One back-to-front list of every sprite (opaque, masked, and translucent), every translucent mesh draw, and, when D8 exists, particles. The list is keyed by quantized view depth (section 6). Sprite ties at that depth break by `(sortingLayer, orderInLayer, sort-axis, stable id)`. Translucent meshes are in this list, drawn where their quantized depth puts them. They are not a later pass that runs after every sprite. Particles join this list. A masked sprite writes depth inside the list, so a later entry whose fragments fall behind that depth fails the test. That is what Q7 means by later transparents and particles occluding correctly.

Sprite entries compare with the convention's nearer-or-equal op. Opaque and masked sprites write depth. Translucent sprites test and do not write. Translucent mesh entries keep the mesh nearer-wins op and do not write depth. Their direct-draw records stay direct draws; the sort that orders them is this list, and the old `renderOrder`-then-depth sort (`GpuSceneSync.hpp:380-388`) is not a second ordering pass.

Depth convention (C15). One value, `DepthConvention::ForwardZ`, lives beside the DEPTH CONVENTION block (`SceneCamera.hpp:120-129`). F5 selects that value and no other. Every depth clear and every depth compare is derived from it:

| Derived | ForwardZ, what F5 ships | Where that value sits today |
|---|---|---|
| Clear | `1.0` | `kDepthClear` (`MeshNode.cpp:83`), local to that file until this convention replaces it |
| Nearer wins | `CompareOp::LESS` | Mesh pipelines (`MeshNode.cpp:832`), pick meshes (`PickOutlineNodes.cpp:582`) |
| Nearer or equal | `CompareOp::LESS_EQUAL` | Sprites, the pick pass for sprites, the 2D grid, the 3D grid (`GridNode.cpp:375`) |

The other value of the same enum is reverse-Z: clear `0`, nearer-wins `GREATER`, nearer-or-equal `GREATER_EQUAL`, and the projection's near/far mapping flipped. Selecting it is that one assignment. F5 selects ForwardZ. The enum lives in `SceneCamera.hpp`, which does not include NRI. The render layer is what maps the enum onto `nri::CompareOp` and the clear float, and every pass reads that mapping. Plan 1's mesh depth load-op clears to the convention's clear.

The mesh row stays the 240-byte `GpuInstance`. Sprite rows are a second struct in the same GPU scene (section 6). They are coarse-culled by the same `VisibleSet` and then CPU-sorted. They do not enter `mesh-cull`'s indirect batches: an indirect batch grouped by mesh cannot express the key order. `mesh-cull` stays the opaque/masked mesh path.

Post and tonemap read scene colour and do not read or write the world depth. The depth transient's lifetime extends through the overlay. The overlay samples it as a texture. The overlay's target is the tonemap target (display-referred), so overlay primitives are display-referred when they land: each one is passed through the tonemap operator alone, and never through the post chain, and the conversion excludes exposure (C15). The operator today is Narkowicz ACES then the true sRGB encode (`tonemap.hlsl:36-56`), which has no exposure term. When D1 adds exposure, it scales scene colour before that operator and does not scale an overlay primitive. Scene colour still goes through the post chain and then the tonemap. A post chain therefore grades the world and does not grade the grid. That is the bug named in Q4. Moving the grid from a blend into linear scene colour, before the nonlinear tonemap, to a blend after that tonemap changes the grid's pixels even when the post chain is empty (C3). That move is an explained re-bless, not a pixel-identity claim. The witness in section 7.1 pins the bug: the same grid pixel with a post chain and without one is equal.

Overlay primitive classes:

| Primitive | Test | Where it sits relative to world colour |
|---|---|---|
| 2D editor grid | Depth-tested with the convention's nearer-or-equal op, no write, once sprites write depth (plan 3). In plans 1–2 sprites write no depth, so the 2D grid is not yet occluded by them. | After tonemap. Leaving the batcher's layer 0 is part of the explained grid diff (section 7). |
| 3D editor grid | Depth-tested with the convention's nearer-or-equal op, no write. Today that op is `CompareOp::LESS_EQUAL` (`GridNode.cpp:363-375`). | After tonemap, over the scene where the depth test passes. Same occlusion against opaque and masked meshes as today. |
| Physics debug, camera rect | Always on top. | After tonemap. They are already drawn after sprites (`EditorAppFrame.cpp:1893-1981`). |
| Gizmo lines | Always on top. | The pixel positions `GizmoDrawSink` already produces (`EditorAppFrame.cpp:1986-1990`). Those pixels are already outside the post chain; plan 1 keeps them. |

`Batcher2D` remains the CPU producer of overlay and screen primitives (Q4). It stops being the producer of `SpriteRenderer` draws in plan 3.

Screen UI is unchanged in order and in target: `gameui`, then pick/outline, then host `imgui`, all on the tonemap target, all after the overlay. The chrome graph (editor ImGui with no scene) declares no world pass and no overlay.

### 3.2 Reserved D1 pass slots

Declared now, built by D1 except where a row says F5. This replaces the 2026-09-16 G1 order. The separate "2D-world" stage after translucency is gone. Sprites sit in the translucent-world slot's one list, with translucent meshes and later particles (C7).

| Slot | Who builds it | F5 |
|---|---|---|
| G-buffer | D1 | Declared, not built. |
| Lighting | D1 | Declared, not built. Lambert runs inside the forward world pass until then. |
| Forward world | F5 | Opaque and masked meshes, front-to-back, indirect. |
| Translucent world | F5 for the shared list; D8 adds particles into that list | One back-to-front list: all sprites, translucent meshes, later particles. Masked sprites write depth inside it. |
| Post | already shipped | Scene colour only. |
| TAA and upscale | D1 | Reserved. The overlay note in section 3.4 sits on this slot. |
| Tonemap | already shipped | Scene colour only. Exposure, when D1 adds it, scales this slot's input and does not scale the overlay. |
| Overlay | F5 | Section 3.1. Excludes exposure. |
| Screen UI | already shipped | After the overlay. |

F5 implements the forward world path, the translucent-world list, the post/tonemap path that already exists, and the overlay. The forward slot and the translucent slot are the two phases of the one raster pass in section 3.1. They share one depth target. The pass does not clear depth between them. F5 leaves the G-buffer and the lighting slot declared and empty.

### 3.3 Render origin

Every position in the packet and in both GPU-scene row types is relative to a per-view render origin. F5 stores that origin as 0, so the values are world positions. The field exists so D17 does not invent a second row layout.

`prevModel` and `model` are relative to the same origin. A rebase (D17) rebases both, in the same extract, including the previous frame's stored `prevModel`. F5 does not rebase, and it does not rewrite rows into per-frame camera-relative positions: that would dirty every row whenever the camera moves. D17 chooses tile-and-offset versus rebasing. The row layout does not.

Culling extracts from the unjittered view (`Frustum.hpp:15-16`). With no jitter, that is the view itself.

### 3.4 Overlay depth and the TAA slot

The overlay reads scene depth at the resolution of the tonemap target, taken from the unjittered world view. F5 has no jitter and no upscale, so that depth is the world-pass depth.

The reserved TAA/upscale slot sits after the translucent world and before the overlay. When D1 fills it, that slot is what publishes the depth the overlay samples: unjittered, output resolution. The overlay does not sample the jittered depth and does not sample a pre-upscale depth. Jitter is applied to the world projection after the unjittered view has been handed to culling and to the overlay. `prevModel` on mesh rows, and the same field on sprite rows, is the velocity input; F5 still has no velocity target and no shader that reads the field.

### 3.5 Source 2 / Deadlock, and Unreal where the user pointed at it

The visual target is Deadlock / Source 2 the renderer (`docs/research/2026-08-12-deadlock-render-target.md`, `docs/research/2026-09-14-engine-ceiling-deadlock-and-box3d.md`). The material *shape* follows Unreal, by the user's direction of 2026-10-02. No Source 2 source is consulted. Unreal is read-only reference, as the 2026-08-12 policy already says.

| | Source 2 / Deadlock | This spec |
|---|---|---|
| World | One deferred renderer. G-buffer, then lighting, then a forward path for translucency and special passes. | The slot order in section 3.2 matches that shape. F5 fills the forward world path and leaves the G-buffer and lighting empty. |
| 2D in the world | Scene primitives through the world renderer, not a second batcher. | Sprites are GPU-scene instances in the world pass, in the one back-to-front list with translucent meshes and later particles. |
| UI | Panorama composites apart from the world colour. | Screen UI stays after tonemap. The overlay is a third thing: editor and debug primitives, depth-tested or always on top, also after tonemap. |
| AA | No MSAA. TAA-family only. | TAA is reserved (G2) and not built. No MSAA is added. |
| Materials | Feature combos on `.vmat`, static and dynamic. | One Surface domain, permutations chosen by usage. The output pins are Unreal's, because that set is what a deferred lighting pass and a forward pass can share. That is the deliberate divergence from copying Source 2's combo language. |
| Lighting | Deferred GGX, baked GI. | Lambert from BaseColor until D1 swaps GGX inside the same template. The ceiling doc's rule stands: no second, half-PBR model beside `mesh.hlsl`. |

Unreal's Paper2D is the precedent for "a sprite is a mesh-pipeline primitive under an orthographic camera." Unreal's split of domain / shading model / blend mode, its vertex factories, and its specular default of 0.5 are the material contract in section 5. One divergence from Unreal is deliberate and is Q7, kept by C7: masked and translucent sprites share one sort key, and that key's depth component is the same quantized view depth the translucent meshes and later particles sort by. Both sprite blends use that key at the same z, and a translucent mesh is an entry in the same list, drawn at its own quantized depth.

### 3.6 One world view

One frame, one camera (C5). The world pass renders with that camera and one depth. A camera that is not the world view is not drawn into that graph.

One function replaces the frame's use of `ActiveSceneCamera` and `ActivePerspectiveSceneCamera`. Precedence, first match wins:

1. **Editor Edit.** The editor camera, 2D or Persp. Scene cameras are not consulted. Sprites and meshes already share this camera (`EditorAppFrame.cpp:1685-1688`).
2. **Runtime and Editor Play.** The active scene camera with `Camera::primary == true`. If more than one active camera is marked primary, the winner is the one with the lowest `Identity::id` (`Components.hpp:286`), ordered by `Guid::operator<` (`Guid.hpp:47-48`: `hi`, then `lo`) (C15). Entity id does not enter the comparison. Registry order does not enter the comparison. An active primary with no `Identity` loses to every active primary that has one. If every active primary lacks an `Identity`, step 2 matches none and precedence continues at step 3. The cameras that lose are not drawn. Extract warns once.
3. **No primary.** The first active orthographic camera, which is the sweep `ActiveSceneCamera` does today (`SceneCamera.hpp:57-112`, installed by `PushSceneCamera` at `RuntimeApp.cpp:856-861`).
4. **No active orthographic camera.** The first active perspective camera, which is the sweep `ActivePerspectiveSceneCamera` does today (`SceneCamera.hpp:206`).
5. **No active camera.** No world view. The frame does not invent one.

`Camera` gains `bool primary`, default `false`, a reflected field. A scene authored before the field loads with every camera not primary and hits step 3 or 4, which is today's split collapsed into one winner. The ABI bump is numbered at execution, in plan 3, the plan that reads the field.

Plans 1 and 2 do not change which camera draws. The host frame builders stop calling `ActiveSceneCamera` and `ActivePerspectiveSceneCamera` in plan 1 (section 4.4). Extract calls them, and through plan 2 it still applies today's split: sprites on the orthographic camera, meshes on the perspective camera. Plan 3 is when extract switches to the precedence above.

An orthographic world view is `Ortho2DView` (`RenderViewSettings.hpp:33-36`). Its near is `-render.ortho2D.depthRange` and its far is `+render.ortho2D.depthRange` (`RenderViewSettings.hpp:21`, default `1000`). Those planes are in camera view space: `glm::orthoRH_ZO` clips after the view matrix (`ViewTransform.hpp:139`) (C15). The settings comment at `RenderViewSettings.hpp:21` describes the range as metres either side of Z = 0. Under this spec that Z is the view's Z. F5 does not add a second reject on world Z. Today's `Ortho2DView` translates in XY only (`ViewTransform.hpp:138`) and `ActiveSceneCamera` drops the entity's Z (`SceneCamera.hpp:87-92`), so with that view the planes lie on world Z = ±`depthRange`. A view whose translation includes Z carries the slab with the camera, and the witness tests view-space Z. Content outside the slab is clipped, as sprites are today. A mesh drawn through an orthographic world view uses the same near and far. The editor's 2D camera already builds its view through `Ortho2DView`, so Edit + 2D uses the same slab.

`main.arcscene` (the `runtime-scene` golden) relies on two cameras today. Entity "Scene" (`:12-18`) is active orthographic, `orthographicSize` 2, and `ActiveSceneCamera` draws the sprites through it. Entity "PerspectiveCamera" (`:292-298`) is active perspective, eye at z = 5, fovY 50, near 0.05, far 500, and `ActivePerspectiveSceneCamera` draws the two `MeshRenderer`s through it. The golden is that composite.

Plan 3 makes the scene's intent explicit (C17). "Scene" gets `primary: true`. "PerspectiveCamera" stays active and is not primary, so it is not drawn. What changes in `runtime-scene`, and only in plan 3:

- Every sprite pixel stays put. The orthographic framing is the one those sprites already have, so the re-blessed golden is also the sprite-parity evidence for this scene.
- The two meshes leave the perspective lens and are drawn with that orthographic view: center at the Scene entity's XY, half-height 2 m, near and far the view-space slab ±`render.ortho2D.depthRange` (default 1000 m). Perspective foreshortening of those two meshes is gone. A mesh whose view-space Z falls outside that slab is clipped, the same rule a sprite already has.

That is a small explained re-bless of `runtime-scene` on both backends, required in plan 3, confined to those two meshes. Plans 1 and 2 still draw those meshes with `ActivePerspectiveSceneCamera`, so the golden does not move then.

Perspective coverage is a separate witness (C17). The plan-3 "sprite occluded by a mesh" witness, or one small new scene if that witness cannot carry a perspective camera, runs through a perspective camera on both backends. A mesh in front of a sprite hides it. A sprite in front of a mesh draws over it. That is the billboard and marker case. `runtime-scene`'s orthographic picture is the sprite-parity evidence; it is not this witness.

`f3_cull_blend.arcscene` also has both cameras active. Its witnessed picture is the perspective camera (`:66-72`, eye at z = 7). Plan 3 sets that camera `primary: true`. The orthographic camera (`:27-33`) stays active and is not the world view. Selecting the same perspective camera should move no pixel. An explained re-bless of `f3-cull-blend` is required only if a pixel does move. Plans 1 and 2 do not edit the scene.

Aphelyon's scene has one active orthographic camera and no second camera. Step 3 selects it. Its picture stays (Q1). Plan 3 does not edit that scene.

`SceneVisibility::views` keeps the ability to hold more than one set (`VisibilitySystem.hpp:88`). F5 fills one. Further views are the multi-view seam (section 9).

### 3.7 Hosts that do not render

Extract runs only for a frame that will draw. ArcaneServer and any other host that does not record a graph do not call it and do not build a packet. A headless runtime does draw (`RenderFrameOffscreen`); it runs extract.

---

## 4. The extract boundary

### 4.1 The stage

One game-thread stage, called where `PrepareSceneForRender` is called today: after simulation has published `WorldTransform` and `WorldBounds`, before `DeclareGraphFrame`. It is the only stage that reads the registry for rendering.

It writes a `RenderFramePacket`. After it returns, the packet is immutable. It holds no `Astra::Registry`, no entity pointers, and no spans into component storage. Spans point at storage the packet owns. The GPU never reads packet memory (C13). GPU-scene build copies row bytes into `NriUploadRing`, which already keeps `FramesInFlight()` persistent-mapped slots (`NriUploadRing.hpp:21`, `:135`, `NriUploadRing.cpp:102`). Those copies are what the GPU reads. Each recording graph also keeps `FramesInFlight()` packets, so a CPU span recorded with a frame (post-chain bytes, ImGui draw data, overlay primitives) is still that frame's span while the frame is in flight. `kMaxFramesInFlight` is 3 and sizes arrays (`FramePacing.hpp:38`). `FramesInFlight()` is the latched depth (`FramePacing.hpp:42`). `render.framesInFlight` defaults to 2 (`RenderDeviceSettings.hpp:80`) and its range is 2..3 (`:109`). Extract writes the slot that is that many extracts old. Everything after extract — GPU-scene build, cull, every node, the overlay — reads the packet.

The packet carries:

| Field | Contents |
|---|---|
| World view | The view from section 3.6, its viewport, its projection, and its render origin (0). |
| Mesh rows | The staged `GpuInstance` rows, batch table, and draw args that `GpuSceneSync` / `BuildGpuSceneFrame` produce today, positions relative to the origin. |
| Sprite rows | Section 6. Empty in plans 1 and 2; the packet still carries the drained world quads those plans record through `Batcher2D`, so the pixels do not move. |
| Visibility | The `VisibleSet` for the world view. |
| Overlay primitives | 2D grid lines, 3D grid description, physics-debug lines, camera-rect lines, gizmo lines. Each primitive has a flag: depth-tested or always-on-top. |
| Pick drawables | The `PickDrawable` span `CollectPickables` builds today. Each sprite drawable carries the section 6.1 key (C2). |
| Post, tonemap inputs, UI | The post-chain bytes, the ImGui draw data, the pick selection. These are already snapshots; extract is where they enter the packet. |
| Light | The default directional light `RuntimeFrame.cpp:676-681` already resolves. No light component is added. |

GPU-scene build and cull become pure functions of the packet. `GpuSceneSyncNode` already refuses to invent rows the CPU did not stage; that stays.

### 4.2 Registry reads that move

These are the render-related registry reads in this tree. Each one moves into the extract translation unit. The line is the read, as re-verified.

| Site | What it reads | Notes |
|---|---|---|
| `RenderSystems.hpp:50-59` | `RenderContext2D`, `SpriteTable`, `SpriteMaterialTable`, `PhysicsInterpBuffer`; view `WorldTransform + SpriteRenderer + Not<Hidden>` | Sprite submission. The body through `:250` becomes extract's sprite-row builder in plan 3. In plans 1–2 extract still performs this read and the packet feeds `Batcher2D`. |
| `GpuSceneSync.hpp:92-111` | `MeshTable`, `MeshMaterialTable`; view `WorldTransform + WorldBounds + MeshRenderer + Not<Hidden>` | Mirror reconcile. |
| `GpuSceneSync.hpp:203-227` | `Changed<>` views of those three since `lastSyncTick`; per-row `GetComponent` | Dirty tracking. `BuildGpuSceneFrame` (`:285-391`) already avoids the registry except a `MeshTable*`. Extract copies the section ranges the builder needs into the packet, so the builder drops that pointer too. |
| `VisibilitySystem.hpp:97-129` | `SceneVisibility`; view `WorldBounds + Not<Hidden>` | `BuildVisibleSet` moves. `MainVisibleSet` becomes "the packet's set". |
| `GpuSceneHost.hpp:87-111` | `GetResource` / `EmplaceResource` of `SceneVisibility` and `GpuSceneMirror`; `GetResource<MeshTable>` | `PrepareSceneForRender` is the extract entry, or extract calls it. It stops being a second door. |
| `PickEmit.cpp:32-185` | `SpriteTable`; sprite view; `PhysicsResource`; collider view; `GetComponent<WorldTransform>`; `MeshTable`; mesh view | Pick drawables in the packet. |
| `SceneCamera.hpp:57-94` and `ActivePerspectiveSceneCamera` (`:206`) | `Camera`, `WorldTransform`, `Transform` | The functions stay in ArcaneCore. Extract becomes their only frame caller. `PushSceneCamera` (`RuntimeApp.cpp:846-861`) becomes "ask extract for the view and store it", at the same point in the frame, so gameplay that already ran Update is undisturbed. |
| `EditorAppFrame.cpp:1898-1922` | `PhysicsResource`, `RenderContext2D`, `PhysicsInterpBuffer` | Physics-debug primitives. The editor passes the flags (play mode, overlay mode, selection). |
| `EditorAppFrame.cpp:1953` | `ActiveSceneCamera` for the camera rect | The rect's corners are computed in extract. |

`Batch2DNode::Prepare` / `Record` already read drained spans and not the registry (`Batch2DNode.cpp:1254` onward, `:1390`). Nodes stay that way.

### 4.3 The include test

A Catch2 test or a scripted build check compiles each translation unit under `ArcaneClient/src/Arcane/Render/` (the render-node TUs and the other TUs in that tree) and reads the preprocessed include tree, MSVC `/showIncludes` (C15). It fails if any preprocessed include, including one reached only through another header, is `Astra/Registry/Registry.hpp`, except in the extract translation unit. A scan of raw `#include` lines is not this test. The same check fails if one of those TUs, other than the extract TU, names `Astra::Registry`. `ArcaneClient/src/Arcane/Render/Nri/` has no allowlist entry: a node that grows a registry include fails the test. The extract TU is the one allowlisted file. The test is the gate that the list in section 4.2 stays moved inside `ArcaneClient`.

### 4.4 The host frame-builder gate

The include test does not see the hosts (C1). Frame-building code obtains render data only through the extract API. A second gate, a Catch2 test or a scripted source check, fails if any named frame builder below reads the Astra registry for render data: a `CreateView`, `GetComponent`, `GetResource`, or `InspectEntity` on that registry, or a call to `ActiveSceneCamera`, `ActivePerspectiveSceneCamera`, `CollectPickables`, `PrepareSceneForRender`, `BuildVisibleSet`, or `GpuSceneSync`.

Named frame builders:

| Function | File |
|---|---|
| `RenderGraph` | `ArcaneRuntime/src/RuntimeFrame.cpp` |
| `EditorApp::RenderSceneToViewport` | `ArcaneEditor/src/App/EditorAppFrame.cpp` |
| `EditorApp::SubmitSceneToBatcher` | `ArcaneEditor/src/App/EditorAppFrame.cpp` |
| `EditorApp::ArmGraphViewportFrame` | `ArcaneEditor/src/App/EditorAppFrame.cpp` |
| `MaterialPreviewHarvester` harvest (`Impl::Harvest`) | `ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp` |
| `ShaderEditorDocument::RenderGraphPreview` | `ArcaneEditor/src/Documents/ShaderEditorDocument.cpp` |

The thumbnail harvest and the shader-editor preview are the frame builders `EditorApp::PumpEditorDocuments` reaches (`EditorAppFrame.cpp:2284-2286`). Their bodies are not in `EditorAppFrame.cpp`. The gate names them anyway, because a registry read that builds their picture is the same hole.

Allow-list, and nothing else inside those functions:

- `GpuSceneInvalidate` in `RenderGraph`, after the frame outcome is not Presented (`RuntimeFrame.cpp:756`). It bumps the mirror generation so the next extract restages. It does not read components to build the picture.
- Mapping the editor selection onto pick ids already in the packet (`ArmGraphViewportFrame`, the loop at `EditorAppFrame.cpp:2177-2181`). The selection is editor state. The drawables came from extract.

Reads outside those functions stay legal. Gizmo transform edits, the outliner, clipboard, scene save, and gameplay `Update` are not frame builders. `CVarRegistry` is not the Astra registry. A new frame builder is added to the table in the same change that adds the function.

---

## 5. The Surface material model

This is the 2026-10-02 direction, including the parts that direction had parked. F5 implements all of it.

### 5.1 Settings

Three independent settings. The user never authors a vertex factory.

| Setting | Values | Meaning |
|---|---|---|
| Domain | `Surface`, `Post` | What the material produces. UI is a later domain (D7), not a value F5 adds. |
| Shading model | `Unlit`, `Lit` | Lit in F5 is Lambert from BaseColor (section 5.4). |
| Blend mode | `Opaque`, `Masked`, `Translucent` | `MaterialBlendMode::Transparent` (value 2) is Translucent. The enumerator may be renamed in the same ABI bump; the numeric value stays 2. Load accepts the old token `transparent` and the new token `translucent`. Save writes `translucent`. |

`MaterialSurface::{Fullscreen, Sprite, Mesh}` (`MaterialSource.hpp:72`) is replaced by `{Surface, Post}`. On-disk `.arcmat` kind `sprite` or `mesh` loads as domain Surface. Kind `fullscreen` loads as domain Post. New files write `surface` or `post`.

A sprite with no material, and a migrated sprite material that does not set a shading model, is **Unlit + Translucent**. That is today's alpha-over sprite (`Batch2DNode.cpp:1315-1316`), which is what Q1 requires for Aphelyon. Masked is the blend an author picks for cutout. A migrated mesh material is **Lit**, with the blend and two-sided flag the section already has.

Instances inherit the domain from the parent and have no domain field (`MaterialAsset.hpp` already treats `parent` as the instance link). Switching a material's domain keeps the graph and never drops a node (C16). Nodes that do not fit the new domain surface as compile errors in the Problems panel. Error-node chrome, greyed pins, and editor polish are the later material-editor mini-arc, step 7. Section 8.2 is the cut: plan 2 does not build that chrome.

### 5.2 Outputs

One output struct, forward- and deferred-agnostic. Unconnected defaults:

| Output | Default | F5 lighting reads it |
|---|---|---|
| BaseColor | `(1,1,1,1)`, times the sampled texture when one is bound | Yes. |
| Metallic | `0` | No. D1's GGX will. |
| Specular | `0.5` | No. The Unreal default, stored so D1 does not invent the pin. |
| Roughness | `0.5` | No. |
| Emissive | `0` | Added when the pin is connected. Unconnected, adding 0 is a bit-exact no-op on today's finite colours. |
| Opacity | BaseColor.a | Translucent blend. |
| OpacityMask | BaseColor.a | Masked clip against the existing cutoff (`boundsMax.w`). |
| Normal | unconnected | Unconnected leaves the vertex factory's geometric normal in place. The pin is tangent-space. A connected Normal on a vertex factory with no tangent frame is a compile error (the same "node does not fit" rule). No F5 default material connects it, which is what keeps `mesh.hlsl` pixels. |
| AO | `1` | No. |
| WorldPositionOffset | `0` | Applied in the vertex factory. Zero leaves positions unchanged. |

The default mesh lit colour is today's formula (`mesh.hlsl:184-206`):

```
albedo = sampled(UV0) * VertexColor * InstanceTint
lit    = albedo.rgb * (ambient + lightColor * saturate(dot(N, L)))
```

with `N` the geometric normal, `L` the existing default light, and alpha `albedo.a`. Opaque returns that. Masked clips on `alpha - cutoff` before it (`mesh.hlsl:216-228`). Translucent returns it with straight alpha and the existing alpha-over blend. Invalid material slot stays the flat path (`mesh.hlsl:192-203`): no sample, `VertexColor * InstanceTint` only.

The default unlit sprite colour is `sampled(UV0) * VertexColor * InstanceTint`, alpha-over, which is `sprite.hlsl:68-70` once the factory feeds the product below.

### 5.3 Vertex factories

Two factories. A material compiles once per factory it is actually used with. Usage chooses. The user does not.

| Input | Sprite factory | Mesh factory |
|---|---|---|
| VertexColor | White for a generated quad (no per-vertex paint exists on `SpriteRenderer` today). | White. |
| InstanceTint | `SpriteRenderer.tint`. | The row's `baseColor` (the multiply at `mesh.hlsl:203`). |
| UV0 | The atlas rect from the sprite asset. | The mesh UV. |

The product `VertexColor * InstanceTint` is the "tint × vertex colour" the sprite factory supplies. For every sprite authored today the vertex colour is white, so the product is the tint, and the pixel matches the batcher. A later gradient can set `VertexColor` per vertex without a new material setting.

The sprite factory also carries the shape (`Rect`, `Circle`, `Capsule`). `Rect` is the quad. `Circle` is the same SDF disc `CircleWorld` rasterises today (`RenderSystems.hpp:185-188`). `Capsule` is the band quad plus two discs, three instances, sub-index 0, 1, 2 in today's submission order (`RenderSystems.hpp:202-207`). They share the sort key, so they stay contiguous.

### 5.4 Interim Lit, and D1

Lit means Lambert from BaseColor, as above. It does not read Metallic, Specular, Roughness, Normal (beyond the geometric default), or AO. D1 replaces the body of the Lit template with GGX and re-blesses every lit golden. Unlit goldens stay. The template, the pins, the vertex factories, and the `.arcmat` settings do not change in that swap.

### 5.5 The six mesh shaders

`MeshNode`'s six pipelines (`MeshNode.hpp:11-16`) become the default Surface material compiled for the mesh vertex factory: Lit × {Opaque, Masked, Translucent} × {one-sided, two-sided}. Plan 2 deletes the fixed `mesh.hlsl` pixel paths as the source of those permutations and must reproduce them under the golden comparator on both backends (section 7). The PSO set stays six until a material's usage asks for another permutation. The live set F5 expects to compile is those six, plus Unlit Translucent for the sprite factory (the default sprite), plus Unlit Masked and Lit Translucent for the sprite factory when a material actually uses them. A permutation nobody uses is not compiled.

### 5.6 Editor

Plan 2's material editor opens and edits a migrated material, and it sets domain, shading model, and blend mode (C16). An instance does not show domain. A domain switch keeps every node. A node that does not fit is a compile error in Problems. The editor does not draw error nodes, and it does not grey pins; those two, and the polish around them, are step 7 (section 8.2). The mesh-material page, which today says the material is not compiled (`golden-gate.ps1:369-371`), compiles the Surface template from plan 2 on. Its golden is re-blessed only with that explained diff, in plan 2, if the compiled preview is part of the picture; the page's job in plan 2 is to show the same pixels the six shaders produced.

### 5.7 Previews

Plan 2 owns this contract (C4). Material thumbnails (`MaterialPreviewHarvester::Impl::Harvest`, `MaterialPreviewHarvester.cpp:1137`) and shader-editor previews (`ShaderEditorDocument::RenderGraphPreview`, `ShaderEditorDocument.cpp:1662`) each render in their own offscreen graph.

A sprite-surface preview is one sprite GPU-scene row through the Surface sprite vertex factory, in that graph's world pass, before post and before tonemap. That is where today's `QuadMaterial` sits (`MaterialPreviewHarvester.cpp:1191`, `ShaderEditorDocument.cpp:1714`). One row, Unlit Translucent unless the material says otherwise, the sprite factory's UV covering the quad.

The checkerboard stays a world quad in that same world pass, drawn before the sprite row. It is not an overlay primitive and it is not a `SpriteRenderer`. Today's harvest paints the checkerboard and the quad into one linear canvas, and a fullscreen material's post chain samples that canvas as `kSceneInput` (`MaterialPreviewHarvester.cpp:1154-1158`, `ShaderEditorDocument.cpp:1699-1700`). A world quad keeps both in scene colour, so the preview's post chain and the tonemap see the composite they see today. An overlay checkerboard would blend after the nonlinear tonemap, which is a different picture (the difference C3 names for the grid).

Mesh-surface previews stay the lit sphere they already draw through the mesh path, in the same world pass. Fullscreen materials stay a post chain over the checkerboard canvas.

Invalidation is the existing preview cache. `MaterialPreviewHarvester::Invalidate` (`MaterialPreviewHarvester.cpp:532`) drops the refusal memo and re-queues a harvest, and keeps serving the last-good thumbnail until the new one lands. Plan 2 calls that `Invalidate` when a sprite material is recompiled. The shader-editor preview rebuilds on the same recompilation, through the document's existing rebuild into `RenderGraphPreview`.

Witness, plan 2: recompile a sprite material and its thumbnail changes to the recompiled shader. The last-good image is not left in place after the harvest lands.

---

## 6. Sprites in the world

A sprite row is a GPU-scene struct of its own. It is not a widening of the 240-byte `GpuInstance` (`GpuSceneTypes.hpp:67`, `gpu_scene.hlsli:1-4`).

The rows persist in the GPU-scene mirror, keyed by entity, the way mesh rows persist in `GpuSceneMirror::slots` (C14). A capsule owns three consecutive rows under that entity, sub-index 0, 1, 2. The per-frame sort writes a draw-order list of visible row indices. It does not rebuild the rows into sort order, and it does not move a row to a new slot because its key changed. `prevModel` is the mesh contract (`GpuSceneSync.hpp:15-19`): `lastModel` on the persisted row is the matrix last uploaded, a new row or a rebuild or a teleport stores `prev == model`, and a row uploaded with `prev != model` is re-dirtied next frame. Rebuilding the row in sort order each frame would look like a teleport and would wipe that history.

Fields:

- `model` and `prevModel`, both relative to the view's render origin (section 3.3).
- Atlas UV rect, a bindless texture index, an alpha cutoff, sprite-factory material slot, `InstanceTint`, shape, sub-index (C9).
- The sort-key inputs that are properties of the sprite: `sortingLayer`, `orderInLayer`, sort-axis value, stable id. Quantized view depth is computed at sort time and is not a reason to rewrite the row.
- Flags. Bit 0 of the reserved set is `casts-shadow`, default false, read by nothing in F5.

The bindless texture index is an index into the bindless set the mesh path already allocates (`MeshNode.cpp:608-625`, `AddMaterial` at `:625`). The alpha cutoff is the material's resolved cutoff, the value mesh rows already store in `boundsMax.w` (`GpuSceneTypes.hpp:60`; default `0.5` at `SceneResources.hpp:293`). A masked sprite clips on `alpha - cutoff`. A translucent sprite stores the cutoff and does not clip. Neither field splits a run (section 6.2).

Sprite view depth is `-viewSpaceZ` of the entity origin (the world-matrix translation column), in metres. A parented sprite uses the world matrix after propagation. A physics-interpolated sprite uses the interpolated translation (`RenderSystems.hpp:138-139`). A translucent mesh entry's view depth is the existing `projectedDepth = -viewCenter.z + depthSortBias` (`GpuSceneSync.hpp:324-328`), also metres, larger meaning farther. Both are quantized before they are compared (section 6.1). The sprite anchor stays the entity origin, so two sprites at the same Z tie exactly, including under an orthographic camera, and a bounds-centre offset cannot reorder them. `depthSortBias` is part of the mesh entry's depth only. It is applied before quantization, and it does not place every mesh after every sprite.

### 6.1 The key

One key for every entry in the back-to-front list (Q7 for sprites, C7 for the list). Drawn in this order, first = behind, last = on top. View depth is quantized to 0.1 mm before the sort (C8). The quantum is `1e-4` metres. The integer is `std::llround(static_cast<double>(depthMetres) / 1e-4)`, computed once per entry. The sort compares those integers. It does not compare the floats, and it does not use an epsilon.

| Component | Direction | Who it applies to |
|---|---|---|
| Quantized view depth | Farther first (larger quantum first) | Every entry: sprites, translucent meshes, later particles. |
| `sortingLayer` | Ascending `int32` | Two sprites. Higher layer draws later. Matches `Batcher2D`'s ascending key (`Batcher2D.cpp:424-431`) for every non-negative layer that fits in 16 bits, which is every authored value in ReferenceProject and Aphelyon (both 0). |
| `orderInLayer` | Ascending `int32` | Two sprites. |
| Sort-axis value | Ascending | Two sprites. `dot(origin, camera.sortAxis)`. A zero axis (the default) contributes 0 and does not reorder. |
| `renderOrder` | Ascending | Two translucent mesh draws, after the quantum. This is `translucencyRenderOrder` (`GpuSceneSync.hpp:272`). It breaks a mesh tie. It does not outrank depth, and it does not sort a mesh against a sprite. |
| Stable id | Ascending entity id, then kind, then sub-index | Everyone. Kind order at the same entity is mesh, then sprite, then particle, so a same-depth sprite paints over that entity's translucent mesh and a later particle paints over both. Sprite sub-index is the `uint8` (capsule 0, 1, 2). Mesh sub-index is the section, and the mesh guid orders two sections the way `GpuSceneSync.hpp:385-387` does. |

`sortingLayer` and `orderInLayer` stay `int32` (`Components.hpp:150-151`). The `uint16` cast at `RenderSystems.hpp:159-160` is not carried over. Values outside `0..65535` no longer wrap. Nothing in the authored scenes depends on the wrap.

The camera gains `glm::vec3 sortAxis`, default `(0,0,0)`, a reflected field. The ABI bump is numbered at execution. Y-sort on the +Y-up plane is the axis `(0, -1, 0)`: a smaller world Y sorts later and draws on top, which is the isometric convention for this handedness.

There is no depth bias and no depth-bit budget on sprites. Layer order at the same quantum is the key, for both blend modes. A higher-layer translucent sprite draws after a lower-layer masked sprite and passes the nearer-or-equal compare against the depth the masked sprite wrote, so it lands on top. The inverse draws the translucent one first, then the masked one overwrites it. One rule, both blends.

A displacement whose view-depth change is below one quantum, and which stays inside the same quantum, does not change the integer. A parented sprite and a physics-interpolated sprite therefore keep their authored layer against a sibling that shares that quantum (C8). Jitter smaller than 0.1 mm does not reshuffle the tie.

### 6.2 Drawing

CPU sort of the visible entries of the shared list (sprites and translucent mesh draws; particles when D8 adds them). The sort reads persisted rows and writes draw order (C14). The plan-3 perf witness records frame time and CPU sort time. The gate is section 7.2: at 50k sprites on the desk GPU, the new path's frame time is at most the old path's plus 2 ms (C10). A later GPU sort has to reproduce this key. It is reserved, not built.

Instanced draws in key order. A run is a maximal contiguous span of sprite draws that shares vertex factory, material, shape, and pipeline state. The bindless texture index and the alpha cutoff live on the row, so a run does not split on texture and does not split on cutoff (C9). The list is not regrouped by material first: that would break the key. 50k overlapping sprites of one material are one run even when their textures differ. Aphelyon's art is translucent, so this sorted path is the path that matters (section 10).

Sprite pipelines cull none (C12). A flip is a negative scale: a negative 2×2 determinant is a mirror (`RenderSystems.hpp:91-108`), and today's sprite pipelines already cull none (`Batch2DNode.cpp:750`, `:1317`). Back-face cull would drop that quad. The sprite vertex factory defines the quad's geometric normal as the normalized cross product of the quad's two edges after the model matrix, local +Z transformed by `model`. Unlit ignores it. Lit, when a material opts in, uses it as N while Normal is unconnected, the same rule as section 5.2. A negative scale flips the normal with the winding, and cull none keeps the flipped quad on screen.

Sprite entries compare with the convention's nearer-or-equal op against depth already written: opaque and masked meshes from step 1, and opaque or masked sprites earlier in this list. Opaque and masked sprites write depth. Translucent sprites do not. Translucent mesh draws in the same list use the convention's nearer-wins op and do not write. Default sprites are translucent, so they do not start occluding each other by depth; the key does that, which is today's painter's order.

Circle and capsule stay. They are sprite-factory shapes (section 5.3), Unlit, Translucent, in the same sorted list. Dropping them would fail parity for any scene that uses them to match a collider.

### 6.3 Picking

Picking follows the same list (C2). Today `CollectPickables` emits sprites in registry iteration order (`PickEmit.cpp:32`), which is not the key. From plan 3, each sprite pick drawable in the packet carries the section 6.1 key. The pick pass draws ids in the section 6.1 order, first = behind. Sprites use the nearer-or-equal op. A later sprite at an equal quantum replaces the id. A translucent mesh and a translucent sprite are drawn in that same order, so the entry the list puts in front is the hit. A translucent mesh behind a translucent sprite stays behind in the id buffer (C7).

Meshes that drew in step 1 keep the nearer-wins test the world pass uses for them. A sprite that draws over a mesh is the one the pick returns, and a mesh that draws over a sprite is the one the pick returns.

Witness, plan 3, both backends: two overlapping sprites at equal z on different `sortingLayer` values, and the hit is the higher layer. The same pair with `sortAxis` `(0, -1, 0)` and different world Y, and the hit is the smaller Y, the one that draws on top.

### 6.4 What Batcher2D keeps

After plan 3, `Batcher2D` records overlay primitives and screen-space work: the 2D grid lines, physics debug, the camera rect, and the glyph API. It does not record `SpriteRenderer`, and it does not record material-preview checkerboards or preview quads (those are section 5.7, plan 2). `QuadWorld` / `CircleWorld` for world sprites are gone. The screen `Quad` / `Rect` / `Line` / `Glyph` API stays.

### 6.5 Reserved row flags

`casts-shadow` on the sprite row, default false. A chunked tilemap row kind is reserved beside the sprite row and the mesh row: no fields are allocated and no shader is compiled. World-space UI, when it exists, is a quad through this same path (a later UI domain, or a Surface quad). It is not built. Screen UI is not that quad.

---

## 7. Gates and verification

`arc` can drive a gate when D27 exists. F5 does not wait for it and does not call it. The gates are `scripts/golden-gate.ps1` and ArcaneTests, on D3D12 and on Vulkan.

Goldens are re-blessed only with an explained diff, written in the bless note. An unexplained pixel fails the plan.

### 7.1 Plans 1 and 2 — pixel identity of world content

On both backends, world sprites and world meshes match the existing goldens. The editor grid leaving the linear canvas is an explained re-bless on every grid pixel, including a frame whose post chain is empty (C3): blending the grid into linear scene colour before the nonlinear tonemap differs from running the tonemap operator on the grid alone and blending after it. `editor-ui` and `editor-ui-perspective` carry the boot scene's post process (`main.arcscene:27-31`), so those lanes' grid pixels are part of that same note. The 2D grid also leaves the batcher's layer 0, so a grid line that crosses a sprite is part of the note too.

Physics-debug lines and the camera rect leave the canvas with the grid. If a blessed frame contains them, they are in the same note. The gizmo is already outside the post chain; its pixels stay.

The grid-unaffected-by-post witness pins the bug: the same grid pixel with a post chain and without one is equal. World pixels in that witness still show the post chain. The witness does not claim the empty-chain grid matches today's golden.

Plan 2's extra identity: the default Surface material reproduces the six mesh shaders. `f3-cull-blend` and `W5` stay at `diffCount == 0` on both backends. Lit goldens are not re-blessed in F5. D1 re-blesses them when Lambert becomes GGX.

### 7.2 Plan 3 — parity, then delete

The old world path is deleted only after every one of these has passed.

**Parity.** Every existing 2D scene renders equivalently on the new path and the old path, including ReferenceProject's sprites viewed through the orthographic camera and Aphelyon's one sprite. "Equivalently" is the golden comparator's own thresholds (C11). `CompareImages` (`ImageCompare.hpp:171-177`) runs exact RGB, then `colorDeltaE94` at `1.0`, then the 3×3 flood, then SSIM over 31×31 at `0.99` (`ImageCompare.cpp:291`). A pixel that cascade calls equal does not increment `diffCount`. The golden gate passes neither `--max-diff-pixels` nor `--max-diff-pixel-ratio`, and an unset aggregate budget is zero differing pixels (`HostConfig.hpp:202-204`, `ImageCompare.hpp:210-212`). Parity passes when that comparator reports `diffCount == 0`. F5 adds no pixel epsilon on top of the cascade. Sprite pixels a mesh occludes follow section 6: a closer opaque or masked mesh hides the sprite; a farther one does not. `runtime-scene` keeps every sprite pixel and moves only its two meshes (section 3.6, C17); that re-bless is the mesh-framing class, and the unchanged sprite pixels are this scene's parity evidence. A mesh outside the view-space orthographic slab is clipped, which the slab witness below pins. Where `diffCount` is not 0 and the diff is confined to sprite edges and UV rounding, the plan records an explained re-bless of that class and no other. The grid, physics-debug, and camera-rect diffs stay the C3 class. A plan-2 preview that moved from `QuadMaterial` to the sprite row stays its own explained diff. `f3-cull-blend` and `W5` stay at `diffCount == 0` for the six mesh shaders.

**Stress scene.** Many sorting layers, Y-sorted overlaps, and mixed masked and translucent sprites at the same z. The picture is checked against a CPU oracle of the key in section 6.1, not against the old batcher (the old key cannot express Y-sort or "masked and translucent at the same z"). Each sprite carries a flat colour so the order is visible.

**Perf witness.** 50k overlapping translucent sprites, old path versus new path, on the desk GPU, both backends (C10). Records frame time and CPU sort time. The old path is deleted only when the new path's frame time is at most the old path's frame time plus 2 ms. That 2 ms is the budget. The recorded sort time is the measurement, not a second threshold.

**Witnesses**, each a Catch2 case unless noted:

| Witness | Asserts |
|---|---|
| Y-sort / layer / order matrix | The key order for a table of layers, orders, equal quantized depth, and a non-zero sort axis. |
| Sprite occluded by a mesh | Perspective camera, both backends (C17). A mesh in front hides the sprite; a sprite in front (nearer quantized view depth, nearer-or-equal compare) draws over the mesh. One small new scene if the witness needs its own camera. |
| Translucent mesh behind a translucent sprite | The mesh stays behind. The pick returns the sprite (C7). Plan 3, both backends. |
| Parented / interpolated sprite keeps its layer | A parented sprite and a physics-interpolated sprite, displaced by less than one 0.1 mm quantum and staying inside that quantum, keep the authored layer (C8). |
| Lit opt-in sprite | An Unlit sprite and a Lit sprite of the same BaseColor differ by the Lambert term, and the Unlit one matches the default sprite. The Lit sprite's N is the quad normal (section 6.2). |
| Extract include test | Section 4.3. Preprocessed includes (`/showIncludes`) of the render-node TUs. |
| Host frame-builder gate | Section 4.4. Lands with plan 1, including the preview functions, which already exist and must not grow a registry read. |
| Overlay depth-tested vs always-on-top | One line behind a mesh is hidden when depth-tested and visible when always-on-top. The depth-tested compare is the convention's nearer-or-equal op. |
| Grid unaffected by post | Section 7.1. The empty-chain grid is an explained re-bless. The overlay conversion excludes exposure (section 3.1). |
| Pick follows the key | Section 6.3. Equal quantum, different layer, the higher layer is picked. Y-sort, the sprite that draws on top is picked. |
| Ortho slab boundary | A sprite and a mesh whose view-space Z is just inside ±`render.ortho2D.depthRange` are drawn; the same drawables just outside that view-space slab are clipped. Plan 3. |
| Recompiled sprite thumbnail | Section 5.7. Plan 2. |
| Stress parity | The scene above. |
| Perf, 50k | New frame time <= old frame time + 2 ms at 50k sprites on the desk GPU (C10). Both backends. CPU sort time is recorded. |
| Parity tolerance | `diffCount == 0` under the comparator's own thresholds (C11). An explained re-bless is allowed when the remaining diff is confined to sprite edges and UV rounding. |
| Goldens | Plans 1–2: world sprites and world meshes identical; grid (and any overlay that left the canvas) is an explained re-bless. Plan 3: `runtime-scene` explained re-bless required, sprite pixels unchanged, the two meshes lose perspective; edge/UV rounding is its own explained class. Both backends. |

---

## 8. Phased delivery

Three plans, in this order (Q6). Each plan leaves the tree green on both backends before the next starts. ABI bumps, if a reflected field or a game-visible header changed, are taken in the plan that changes them and numbered against `main` at that time. Known causes: `MaterialSurface` becoming `{Surface, Post}` in a public header, `Camera::sortAxis`, and `Camera::primary`. This branch is ABI 54.

### 8.1 Plan 1 — structure

Plan 1 does not merge sprites into the mesh pass (C6). The graph stays two world nodes, in today's order.

- The graph gains a per-attachment load op and clear value (`NriGraphContext.cpp:1235-1252`).
- The declarative clear is the load-op of the first world node, which is still `batch2d`. Colour loads cleared to `render.clearColor`. `batch2d` stops calling `CmdClearAttachments` (`Batch2DNode.cpp:1405`).
- `mesh` stays the next node. Its depth load-op clears to the depth-convention clear (section 3.1), and its colour load-op is LOAD, so it does not erase what `batch2d` drew. `MeshNode`'s depth `CmdClearAttachments` (`MeshNode.cpp:957`) goes away. The convention value is declared in this plan. Mesh clears, mesh compares, and overlay depth compares read it. Reverse-Z stays unselected.
- World-node order is unchanged: `batch2d` still precedes `gpuscene-sync` / `mesh-cull` / `mesh`. The Q4 overlay still moves to after tonemap in this plan (the 3D grid leaves its pre-post slot and joins it). That move is the explained grid re-bless, not a change to the order of `batch2d` and `mesh`.
- World sprites are still `Batcher2D` draws inside `batch2d`. World meshes are still the mesh pass. That is today's order, so world content stays pixel-identical on both backends. Camera selection is unchanged, so `runtime-scene` and `f3-cull-blend` world pixels stay.
- The single unified world pass, with sprites in it, arrives in plan 3.
- Extract and `RenderFramePacket`. The reads in section 4.2 move. The include test (section 4.3) and the host frame-builder gate (section 4.4) land. The packet's sprite payload is still the batch the batcher drains.
- The overlay pass after tonemap: 2D grid, 3D grid (GridNode folds in), physics debug, camera rect, gizmo lines. Each primitive goes through the tonemap operator alone and excludes exposure (section 3.1). Screen UI stays after it. The grid move is the explained re-bless in section 7.1.
- Non-rendering hosts do not call extract.
- Gate: section 7.1.

### 8.2 Plan 2 — materials

Plan 2's material-editor scope is the bullets in this section (C16). A plan written from this section implements these bullets and does not add editor chrome past them.

- Open and edit a migrated material. Set domain, shading model, and blend mode (section 5.6).
- `.arcmat` load migration. Kind `sprite` or `mesh` loads as Surface. Kind `fullscreen` loads as Post. Instances inherit domain and are not given a domain field of their own.
- A domain switch keeps the graph and never drops a node. A node that does not fit the new domain is a compile error in Problems. That error is the misfit signal this plan ships.
- The Surface template, both vertex factories, the output struct in section 5.2.
- The six fixed mesh shaders become the default Surface material and reproduce today's pixels under the golden comparator, both backends.
- The default sprite factory permutation reproduces `sprite.hlsl` for Unlit Translucent.
- Preview contract (section 5.7): thumbnail and shader-editor sprite previews become one sprite GPU-scene row in their own world pass, checkerboard as a world quad before that row, invalidation through `MaterialPreviewHarvester::Invalidate`. The recompiled-thumbnail witness lands.
- Meshes look identical. World sprites are still drawn by the batcher. Lit is Lambert. No golden is re-blessed for a lighting change. A preview picture that changes because `QuadMaterial` became the sprite row is an explained diff, in plan 2, if that picture is in a golden.
- Gate: section 7.1, plus the six-shader identity, plus the thumbnail witness.

Error-node chrome, greyed pins, and editor polish are the later material-editor mini-arc, which C16 calls step 7. Step 7 is after these three plans. It is not a task in the list above, and a plan-2 task does not grow it from the Problems error. The graph-retention rule and the Problems error stay here; the deferred chrome does not come back with them.

### 8.3 Plan 3 — sprites, parity, retirement

- Sprite rows persist in the GPU-scene mirror, keyed by entity (C14). Each row carries the bindless texture index and the alpha cutoff (C9). Pipelines cull none, and the quad normal is defined for Lit (C12). Circle and capsule stay.
- The section 6.1 key. View depth is quantized to 0.1 mm before the sort (C8). The CPU sort writes draw order and leaves the rows where the mirror put them.
- One back-to-front list of all sprites, translucent meshes, and the particle entries D8 will add (C7). Opaque and masked meshes stay the indirect front-to-back batches. Masked sprites write depth inside the list. Sprite compares use the convention's nearer-or-equal op.
- One world view (section 3.6). `main.arcscene` marks the orthographic "Scene" camera primary. More than one active primary resolves to the lowest `Identity` GUID (C15). `f3_cull_blend.arcscene` marks "PerspectiveCamera" primary and leaves its orthographic camera active. `Camera::primary` and `Camera::sortAxis` land here.
- The unified world pass: sprites leave `batch2d` and join the mesh pass as section 3.1 describes. `batch2d` no longer draws world sprites.
- Picking follows the list (section 6.3): the layer witness, the Y-sort witness, and the translucent-mesh-behind-sprite witness.
- The view-space ortho slab witness (section 7.2).
- The perspective sprite-versus-mesh witness, both backends (C17).
- The parity gate under the comparator's own thresholds (C11), the stress scene, and the 50k witness at old frame time plus 2 ms (C10), all green, both backends.
- Then `Batcher2D` retires from world content (section 6.4). Not before.
- `runtime-scene` re-bless, explained, required: every sprite pixel stays, and the two meshes lose perspective. `f3-cull-blend` stays the perspective picture unless a pixel moves, in which case the note says why.
- Aphelyon's sprite matches the old path.

---

## 9. Reserved seams

Named so a later arc extends them instead of replacing the pipeline.

| Seam | What F5 leaves in place | Who fills it |
|---|---|---|
| Render origin | Per-view origin on the packet and both row types. Value 0. `prevModel` and `model` share it. A rebase moves both. | D17. Tile-and-offset versus rebasing is D17's choice. |
| TAA jitter and velocity | Unjittered cull (`Frustum.hpp:15-16`). `prevModel` on mesh and sprite rows, same origin as `model`. No jitter, no velocity target. | D1, at the slot in section 3.4. The overlay samples the unjittered output-resolution depth that slot publishes. |
| D1 pass slots | The order in section 3.2. G-buffer and lighting declared empty. | D1. |
| Multi-view | `SceneVisibility::views` can hold more than one set. F5 fills the world view only. | Shadows, extra cameras, render-to-texture. |
| Render thread | The packet is immutable and contains no registry. Nodes already read snapshots. | D3 moves everything after extract off the game thread. |
| `casts-shadow` | A flag on the sprite row, default false, not read. | D1 shadows. |
| Tilemap chunk row | A row kind reserved next to mesh and sprite. No struct body, no shader. | The tilemap epic / D11. |
| World-space UI | A quad through this path, later. | D7's UI domain, or a Surface quad. Screen UI is not this. |
| GPU sprite sort | The CPU key in section 6.1, including the 0.1 mm quantum, is the contract. | The CPU sort stays while the 50k witness meets old frame time plus 2 ms. |

---

## 10. Risks and mitigations

| Risk | Why it is real | Mitigation |
|---|---|---|
| Aphelyon's art is translucent, so the sorted path is the main path, not a rare one. | The one Aphelyon sprite is a default sprite, and default is Unlit Translucent. A sort bug is a game bug. | Q7's single key, inside C7's shared list, so a translucent mesh behind that sprite stays behind. The stress scene mixes masked and translucent at the same quantum. Parity runs on Aphelyon's scene before the old path is deleted. |
| CPU sort cost at 50k. | Overlapping translucent sprites do not instance across a material change, and the sort is on the game thread. | The 50k witness gates plan 3 at old frame time plus 2 ms on the desk GPU (C10). GPU sort is reserved on the same key. |
| Vertex-factory permutation count. | Domain × shading × blend × factory × sidedness is a large product. | Compile on use (section 5.5). The live set is the six mesh permutations plus the sprite permutations something actually references. |
| `.arcmat` migration of user content. | ReferenceProject and Aphelyon both have `sprite` and `mesh` kinds on disk. A bad load is a blank material. | Load maps `sprite`/`mesh` → Surface and `fullscreen` → Post. Save writes the new tokens. Instances inherit domain and are not rewritten into a domain of their own. A domain switch keeps the graph and never drops a node. Misfit nodes are compile errors in Problems. Error-node chrome, greyed pins, and polish are step 7 (section 8.2). |
| Golden churn. | The grid leaves the linear canvas in plan 1, and the blend-before-tonemap versus blend-after-tonemap difference moves grid pixels even with an empty post chain. Plan 2 may move a preview picture off `QuadMaterial`. Plan 3 draws `runtime-scene`'s two meshes through the orthographic camera. Sprite edges can move by UV rounding. | Plans 1–2 re-bless the grid class of diff, and plan 2 may re-bless that preview picture, each with a note. The grid-unaffected-by-post witness pins that a post chain does not move a grid pixel. Plan 3 re-blesses `runtime-scene` with the section 3.6 note: every sprite pixel stays, and the two meshes lose perspective. An edge/UV diff is its own explained re-bless under C11, and only when the comparator's `diffCount` is not 0. Lit goldens are not touched until D1. |
| Two cameras in `main.arcscene` and `f3`. | One depth cannot be both projections. | Section 3.6. More than one active primary resolves to the lowest `Identity` GUID. `main.arcscene` marks the orthographic Scene camera primary. `f3_cull_blend.arcscene` marks the perspective camera primary and leaves the orthographic camera active. Plans 1–2 do not change selection, so neither golden moves then. |
| 2D grid coverage. | The 2D grid was batcher layer 0, under sprites. The overlay is after tonemap, so a line can cross a sprite it used to sit under. | Named in the plan-1 bless note, with the tonemap-order difference. The pinned witness is the post-chain half, which is the bug. Plan 3's masked sprites occlude a depth-tested grid; translucent sprites do not write depth, so the grid shows through them. |
| Extract misses a read. | A registry read left in a host frame builder compiles, and a raw `#include` scan misses a header pulled in through another header. | Section 4.2 is the move list. Section 4.3 fails CI when `/showIncludes` on a TU under `ArcaneClient/src/Arcane/Render/` shows the registry outside the extract TU. Section 4.4 fails CI on a render-data registry read in a named frame builder. |
| Circle and capsule parity. | They are not quads. A quad-only row would move every collider-matched sprite. | They are sprite-factory shapes in the same sorted list (section 5.3). |
| Default Lit on meshes, default Unlit on sprites. | Using one default for both would move either the mesh goldens or Aphelyon. | Section 5.1. The template is shared; the setting is not. |

---

## 11. Out of scope

Recorded so a plan does not pick them up.

- The render thread and parallel command recording (D3).
- GGX, the G-buffer, clustered lighting, shadows, GTAO, TAA, the upscaler (D1).
- 64-bit positions and a moving origin (D17).
- Tilemaps, beyond the reserved row kind.
- A UI domain and the ImGui replacement (D7).
- Selecting reverse-Z. Section 3.1 declares the depth convention so that selection is one assignment. MSAA, a velocity target, and GPU sprite sorting.
- Error-node chrome, greyed pins, and material-editor polish. Those are step 7 (section 8.2). Plan 2 ships the Problems compile error and keeps every node on a domain switch.
- A second world camera drawn into the same graph.
- Changing `mesh.hlsl`'s Lambert result in F5. D1 changes it, inside the template, and re-blesses.
- Rewriting Aphelyon content. The one sprite and the one camera are a parity input, not a content task.
- `arc` (D27). Helpful when it exists, not a dependency.

---

## 12. Rulings ledger

Each line is the user's pick on 2026-10-09. The amendments were adopted from the outside review the same day. Q7 replaces the depth-bias sentence that was attached to Q2; the precedence Q2 named is unchanged. C1–C17 are the review-round rulings the same day. C7 is how Q7's "later transparents and particles" stay correct: they share the sprites' back-to-front list.

| # | Pick |
|---|---|
| Q1 | Sprites are Unlit by default. Lighting is an opt-in per material. Aphelyon sees no visual change. This answers G4: no separate 2D lighting path, and T1's light grid stays with D1. |
| Parity | Existing 2D content renders correctly on the new path before the old path is retired. |
| G1 | The 2026-09-16 separate "2D-world" stage is superseded by unification. |
| Q2 | Order is real z, then `(sortingLayer, orderInLayer)`, then an optional per-camera sort axis (Y-sort). |
| Q3 | The 2026-10-02 Surface model, in full, including the parts that note had excluded. One template, sprite and mesh vertex factories, Unreal-shaped outputs, Unlit plus interim Lambert Lit, the six mesh shaders become the default Surface material, `.arcmat` migrates. |
| Q4 | One debug/editor-primitive pass after tonemap, reading scene depth. Each primitive is depth-tested or always-on-top. `Batcher2D` stays the CPU producer. The 3D grid folds in. Screen UI composites after it. A test pins that a post chain does not affect the grid. |
| Q5 | One game-thread extract owns every render-related registry read and writes an immutable packet. Everything after reads the packet. A test enforces that render-side code does not include the registry. No render thread in F5. |
| Q6 | Plan 1 structure, plan 2 materials, plan 3 sprites plus parity plus `Batcher2D` retiring from world content. |
| Q7 | One sort key for masked and translucent sprites: view depth, then `sortingLayer`, then `orderInLayer`, then sort-axis value, then stable id. Instanced draws in that order. `LESS_EQUAL` against meshes. Masked sprites write depth. No depth-bit budget. CPU sort now. GPU sort later. C7 keeps this key and places the sprites in one back-to-front list with translucent meshes and later particles. |
| Amendment | The packet and the rows carry positions against a per-view render origin, 0 until D17. `prevModel` and `model` share the origin. A rebase rebases both. |
| Amendment | The overlay's depth test against TAA jitter and upscale is noted on the reserved TAA slot. Overlays need unjittered, output-resolution depth. |
| Amendment | Dedicated servers and other non-rendering hosts skip extract. |
| Amendment | `VertexColor`, `InstanceTint`, and `UV0` come from the vertex factory. The sprite factory supplies tint × vertex colour and the atlas UV. The mesh factory supplies white and the mesh UV. |
| Amendment | D1 re-blesses the lit goldens when Lambert becomes GGX. |
| Amendment | Plan 2 reproduces the six fixed mesh shaders under the golden comparator. |
| Amendment | Specular stays as a pin with Unreal's default, 0.5. |
| Amendment | No tilemaps exist. A chunked row is reserved. |
| Amendment | A `casts-shadow` flag is reserved on sprite rows. |
| Amendment | Plan 3 gates on the 50k perf witness, the stress parity scene, and per-backend goldens for D3D12 and Vulkan. |
| Amendment | `arc` is helpful and is not a dependency. |
| C1 | Extract enforcement covers the hosts too. Frame-building code in ArcaneRuntime (`RuntimeFrame.cpp`) and ArcaneEditor (`EditorAppFrame.cpp` viewport, thumbnail, and preview frame builders) obtains render data only through the extract API. A gate fails if those frame builders read the registry for render data: a test or a scripted source check over the named functions, plus a documented allow-list for non-render reads, in addition to the `ArcaneClient/Render` include test. |
| C2 | Picking follows the unified sprite sort key. Pick drawables in the packet carry the same key. The pick pass resolves equal-depth overlaps by that key. Witness: two overlapping sprites at equal z on different layers, the visually top one is picked, and the same with Y-sort. |
| C3 | The pixel-identity promise for the grid with an empty post chain is removed. The overlay move is an explained re-bless, because a blend before a nonlinear tonemap differs from a blend after it, pinned by the grid-unaffected-by-post witness. |
| C4 | Preview contract, plan 2. Material thumbnails and shader-editor previews render their sprite-surface preview as one sprite GPU-scene row through the Surface sprite vertex factory, in their own offscreen graph's world pass, before post and tonemap, as today's batch is. The checkerboard backdrop is a world quad in that pass, drawn before the sprite row, so both stay in scene colour for the preview's post chain and tonemap. Preview invalidation follows material recompilation through the existing preview cache. Witness: a recompiled sprite material updates its thumbnail. |
| C5 | One world view per frame. The world pass renders with one camera. The host's main view is the active scene camera resolved by one rule that extends today's `ActiveSceneCamera` / `ActivePerspectiveSceneCamera` split: editor Edit uses the editor camera; otherwise an active `Camera::primary`, else the first active orthographic camera, else the first active perspective camera. Orthographic cameras use `render.ortho2D.depthRange` as near and far, and content outside is clipped, as sprites are today. A slab-boundary witness is required. `main.arcscene` relies on two cameras today; plan 3 marks the orthographic "Scene" camera primary, states the `runtime-scene` change, and requires an explained re-bless. |
| C6 | Plan 1's temporary arrangement: the declarative clear is the load-op of the first world node, `batch2d` stops clearing, node order is unchanged (`batch2d`, then `mesh`, then the overlays), so world content stays pixel-identical. The single unified world pass with sprites in it arrives in plan 3. |
| C7 | The world pass draws opaque and masked meshes first, front-to-back and indirect, then one back-to-front list of all sprites (masked and translucent) plus translucent meshes plus later particles, keyed by quantized view depth. Sprite ties break by (layer, order, axis, stable id). Masked sprites write depth inside that pass. Witness: a translucent mesh behind a translucent sprite stays behind. |
| C8 | View depth is quantized to 0.1 mm before sorting. There is no epsilon compare. Witness: a parented or physics-interpolated sprite keeps its authored layer. |
| C9 | The sprite row carries a bindless texture index and an alpha cutoff. Runs never split on texture. |
| C10 | Absolute perf budget: new frame time <= old frame time + 2 ms at 50k sprites on the desk GPU. |
| C11 | Parity tolerance is the golden comparator's own thresholds (`diffCount` 0 under them). Otherwise an explained re-bless whose diff is confined to sprite edges and UV rounding. |
| C12 | Sprite pipelines cull none, because a flip is a negative scale. The quad normal is defined for the Lit opt-in. |
| C13 | Packet count is `FramesInFlight()`. The GPU never reads packet memory. |
| C14 | Sprite rows persist in the GPU-scene mirror keyed by entity, so `prevModel` works. The sort produces only draw order. |
| C15 | The primary-camera tie-break is the lowest `Identity` GUID. Entity id and registry order are not that comparison. One depth-convention constant drives every compare and every clear, so reverse-Z is one switch. Overlays exclude exposure. The extract include gate scans preprocessed includes (`/showIncludes`) of render-node TUs. The ortho slab is in camera view space. |
| C16 | Plan 2 keeps minimal material-editor support (open and edit migrated materials; set domain, shading, and blend) and the `.arcmat` load migration (Sprite/Mesh → Surface, Fullscreen → Post). A domain switch keeps the graph and never drops nodes. Misfit nodes are compile errors in Problems. Error nodes, greyed pins, and polish are a later material-editor mini-arc (step 7). Section 8.2 states that cut. |
| C17 | The orthographic Scene camera is primary in `main.arcscene`. Every sprite pixel in `runtime-scene` stays, and only the two meshes lose perspective, a small explained re-bless, so the golden is also sprite-parity evidence. The plan-3 sprite-occluded-by-a-mesh witness, or one small new scene, runs through a perspective camera on both backends. |

---

## 13. Amendments applied to the binding docs

Applied in the same change as this file. Each edit points back here.

**`docs/research/2026-09-16-direction-and-sequencing.md`.**

- G1's guard no longer says F5 implements a separate 2D-world stage after forward/transparent. It now says the declared order is G-buffer → lighting → forward world → translucent world → post → TAA (reserved) → tonemap → overlay → screen UI, that F5 implements the forward world path (opaque and masked meshes) and the overlay, and that world sprites, translucent meshes, and later particles share one back-to-front list in the translucent-world slot.
- The F5 bullet in the order list points at this spec: declarative clear, one world pass, one depth, 2D drawn through the 3D path. G1 (as amended) and G2 live here.
- G4's open question is closed: answered 2026-10-09 by F5 Q1, unlit by default, lit opt-in on the same Surface material, no separate 2D lighting path, T1's light grid stays with D1. The step-6 sentence and the "what would change this order" bullet that still treated that decision as open say the same.

**`docs/research/2026-08-21-3d-foundations-assessment.md`, F5 section.**

- A supersession line at the F5 heading: the "both sort into a shared depth" bridge is not the design; this spec is. The clear still moves off `CmdClearAttachments`. The mid-pass clear cite is corrected from `Batch2DNode.cpp:1321-1325` to `Batch2DNode.cpp:1395-1405`. The clear-seam cite is corrected from `NriGraphContext.cpp:1123-1163` to `NriGraphContext.cpp:1235-1252`.
- The open question "does `SpriteRenderer` stay 2D-positioned?" is answered: it is a world quad in the shared depth buffer, drawn through the 3D path. The sequencing line for F5 points here.
- The old numbered shared-depth item carries its own pointer at this spec, so a reader of that item does not have to find the heading banner.

**Material direction of 2026-10-02** (`D:/dev/starworks/Arcane/.superpowers/sdd/2026-10-09-f5-spec/material-direction-2026-10-02.md`).

- The exclusion of parts 3–4 is superseded. F5 implements parts 1, 2, 3, 4, and 5. The file points at this spec. That file lives outside this repository; the sentence is edited there, and this section is the copy that lands in git.
