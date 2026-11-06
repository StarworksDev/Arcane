# Direction and sequencing — decided 2026-09-16 (binding)

**Status:** decision record. Supersedes the 2026-09-03 arc ordering ("rendering closes out before cvars") for everything below; extends, does not replace, the 2026-08-21 foundations pivot, the 2026-08-12 Deadlock render contract and the 2026-09-14 ceiling doc. Where those docs and this one disagree on ORDER, this one wins; on CONTENT (what a tier is, what a foundation is), they win.

## The premise

Aphelyon is the product being developed now, and it is a 2D game. Arcane is its engine and must not foundationally change under it. The user's concern, stated exactly: the "greedy ordering" — building only what 2D needs directly after the 3D foundations, and deferring the 3D renderer tiers — must not force a foundational rewrite of Arcane later.

## The finding: order is not what protects us; the seams are

The Deadlock-class renderer tiers (T1–T7, `2026-08-12-deadlock-render-target.md`) are mostly ADDITIVE. Shadows, fog, the NPR layer and baked GI attach to a pipeline without changing what sits under them. Exactly four things in the 3D target change data formats or touch every render node, and only those can force a foundational rewrite if 2D-era work is built without them. Each has a cheap guard that belongs in a foundation spec, not in code:

| # | The thing | Why it is foundational | Guard (spec-level, cheap now) |
|---|---|---|---|
| G1 | **The deferred split.** T1 is clustered DEFERRED (decided 2026-08-12). | A forward-only compositing design means every 2D-era node later learns where the G-buffer, lighting and forward-transparent passes sit. | **F5 declares the pass slots and the shared-depth contract per the Deadlock target now** (G-buffer → lighting → forward world → translucent world → post → TAA reserved → tonemap → overlay → screen UI) and implements the forward world path (opaque and masked meshes) and the overlay. World sprites, translucent meshes, and later particles share one back-to-front list in the translucent-world slot. The separate 2D-world stage is superseded by `docs/superpowers/specs/2026-10-09-f5-unified-world-pipeline-design.md`. |
| G2 | **TAA's frame contract** (T5): jitter + per-object motion vectors. | Touches every node that draws; particles and animation are exactly the motion sources. | **F3 and F5 reserve jitter and a velocity output as declared inputs.** `PreviousTransform` (F1) already carries the data. |
| G3 | **The cooked asset format.** Baked GI (T2) needs lightmap UVs; skeletal animation needs skin weights; both land in the mesh cook. | The artifact store carries NO engine/format version stamp today (relocation closeout follow-up (c)). Unversioned cooked content is the one thing that gets expensive with volume. | **Stamp the artifact format before Aphelyon content piles up** — the hygiene wave moves UP to right after the foundations. |
| G4 | **The light grid** (T1's cluster grid). | A separate 2D lighting path built before T1 is the greedy trap in its purest form: T1's grid is the same structure 2D lights would use. | **Answered 2026-10-09 by F5 Q1** (`docs/superpowers/specs/2026-10-09-f5-unified-world-pipeline-design.md`): unlit by default, lit opt-in on the same Surface material. No separate 2D lighting path. T1's light grid stays with D1. |

Everything else in T2–T7 waits without compounding cost. The cvar system is engine-wide config plumbing; retrofitting it is mechanical and it stays sequenced where it was.

The standing rule that enforces this (user, 2026-09-04): **every design — asset pipeline and tooling arcs included — states what Source 2 / Deadlock does and why we match or deliberately diverge.** F3 and F5 obey it on paper; that is what makes greedy ordering safe.

## The order

1. **Replication arc v1 — SPEC ONLY** (design sessions, no code). Requirements are in `2026-09-16-multiplayer-shape-and-project-layout.md`: per-entity authority, interest-based replication, a net-driver seam (GameNetworkingSockets = candidate first client driver, after the shape), engine-owned quantized delta serialization. General model first; Aphelyon = thin first consumer. Written now because the decision is two days old and the reasoning is fresh; it is the largest unretired architectural risk (it can still change what ArcaneServer owns vs the services, and whether one module truly serves every net mode).
2. **The last render foundations, in this order: F4 → F3 → F5**, with G1–G3 written into F3/F5's specs.
   - **F4** (editor authoring for 3D; user-scoped 2026-08-23): a SEPARATE editor camera (DCC model); viewport controls to flip 2D ↔ 3D perspective; a 3D grid to ground perspective + a good 2D grid; `.arcmesh` creation onto the normal asset-creation flow. Goes first because no 3D desk pass is honest without an editor camera, and F3's culling wants a real camera to test against.
   - **F3**: per-mesh AABBs, world bounds, frustum culling, draw sorting (opaque front-to-back, transparent back-to-front). The graph today "does not reorder, cull".
   - **F5**: compositing — declarative clear, one world pass, one depth, 2D drawn through the 3D path (`docs/superpowers/specs/2026-10-09-f5-unified-world-pipeline-design.md`). G1 (as amended above) and G2 live here.
   - Also: NRI Phase 4's Task 10 desk items get reworded once F4 makes a camera orbit possible.
3. **Engine hygiene wave** (moved up from after replication v1): the artifact-store format stamp (G3); the Hub project-GUID healing (unmerged; it opened the archive copy by mistake on 2026-09-16); the runtime's exit-5-on-self-healing-refusal witness semantics; the hang watchdog's minidump capture vs in-flight GPU waits; when a second game module exists, whether `source://` should list non-module C++.
4. **Cvars** (last of the engine plumbing, as sequenced 2026-09-03; the cvar spec's own trigger stands — do not "fix" its section 10).
5. **Replication arc v1 — implementation.**
6. **Aphelyon feature work** on the declared seams: UI runtime, particles, animation (2D sprite/skeletal first), and whatever else the game needs. **G4 is answered (F5 Q1, 2026-10-09): unlit by default, so this step does not wait on a lighting decision and T1's light grid does not move ahead of it.**
7. **Linux + CI** (the linux-1 lane, Manifold2D extraction Part B) when ArcaneServer has something to deploy.
8. **T1 onward** (T1 → T5+T6 → T3 lite → T4 lite → T2 lite; scene SDF and VRAD3 parity trail) when 3D is the job that quarter. The 2026-09-14 caution stands: do not start T1 while 2D text is a white texel unless 3D is the only job.

Parked with checkable triggers, unchanged: the `Nri/nodes/` render-domain reorg (fires at T1 / 10+ pass types); Manifold3D (no 3D game consumes Box3D yet).

## What would change this order

- G4 is closed (2026-10-09, F5 Q1): Aphelyon is unlit by default. Lighting the game later is the Surface Lit opt-in and D1's light grid, not a separate 2D path and not a reorder of step 6.
- A replication spec finding that the module cannot serve every net mode → that finding outranks steps 2–4 and is resolved first (the shape is the foundation).
- 3D becomes the quarter's job → step 8 moves ahead of step 6.

## Why not finish T1–T7 first

Two to four months focused (eight to twelve wall clock alongside Aphelyon, per the 2026-09-14 estimates) of work that is invisible to a 2D game, with no compounding cost to deferring it once G1–G4 are in the foundation specs. The only tier content that is genuinely load-bearing for 2D-era work is enumerated above and guarded.
